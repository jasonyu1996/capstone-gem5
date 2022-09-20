#include<stdexcept>
#include<cstring>
#include "mem/packet_access.hh"
#include "arch/riscvcapstone/node_controller.hh"
#include "base/trace.hh"
#include "debug/CapstoneNCache.hh"
#include "debug/CapstoneCapTrack.hh"
#include "debug/CapstoneNodeOps.hh"


/*
 *
 * Note: current implementation is not very performant.
 * The whole revocation node subsystem will be blocked for every
 * request.
 *
 * Might consider optimising this in the future.
 * */

#define CAPSTONE_NODE_BASE_ADDR 0x100000000000ULL
#define CAPSTONE_NODE_N 65536

namespace gem5::RiscvcapstoneISA {

NodeController::NodeController(const NodeControllerParams& p) :
    ClockedObject(p),
    currentPkt(NULL),
    mem_side(this),
    cpu_side(this),
    system(p.system),
    freeNodeInited(0),
    free_head(NODE_ID_INVALID),
    tree_root(NODE_ID_INVALID) {
    DPRINTF(CapstoneNCache, "Size of node = %u\n", sizeof(Node));
}

NodeController::CPUSidePort::CPUSidePort(NodeController* owner) :
    ResponsePort(owner->name() + ".cpu_side", owner),
    owner(owner),
    retryPkt(NULL),
    toRetryReq(false) {
}


bool
NodeController::MemSidePort::recvTimingResp(PacketPtr pkt) {
    owner->handleResp(pkt); // always succeed
    // no need to send retry response because responses are
    // always successfully handled
    return true;
}


void
NodeController::MemSidePort::recvReqRetry() {
    assert(retryPkt != NULL);
    PacketPtr pkt = retryPkt;
    retryPkt = NULL;
    trySendReq(pkt);
}

void
NodeController::sendLoad(NodeID node_id) {
    Addr addr = nodeId2Addr(node_id);
    DPRINTF(CapstoneNodeOps, "send load %lx\n", addr);
    RequestPtr req = std::make_shared<Request>();
    req->requestorId(requestorId);
    req->setPaddr(addr);
    PacketPtr pkt = Packet::createRead(req);
    pkt->setSize(sizeof(Node)); // FIXME: do we need to specify the size here?
    pkt->allocate();

    mem_side.trySendReq(pkt);
}

void
NodeController::sendStore(NodeID node_id, const Node& node) {
    Addr addr = nodeId2Addr(node_id);
    DPRINTF(CapstoneNodeOps, "send store %lx\n", addr);
    RequestPtr req = std::make_shared<Request>();
    req->requestorId(requestorId);
    req->setPaddr(addr);
    PacketPtr pkt = Packet::createWrite(req);
    pkt->setSize(sizeof(Node));
    pkt->allocate();
    memcpy(pkt->getPtr<void>(), &node, sizeof(Node));

    mem_side.trySendReq(pkt);
}

bool
NodeControllerQuery::transit(NodeController& controller, PacketPtr current_pkt, PacketPtr pkt) {
    current_pkt->makeResponse();
    assert(current_pkt->getPtr<NodeControllerCommand>() != NULL);
    current_pkt->deleteData();
    current_pkt->setSize(sizeof(Node));
    current_pkt->allocate(); // TODO: let the cpu allocate
    memcpy(current_pkt->getPtr<void>(), pkt->getPtr<void>(), sizeof(Node));

    return true;
}

bool
NodeControllerAllocate::transit(NodeController& controller, PacketPtr current_pkt, PacketPtr pkt) {
    Node node;
    switch(state) {
        case NCAllocate_LOAD_PARENT:
            node = pkt->getRaw<Node>();
            parentDepth = node.depth;
            nextNodeId = node.next;
            node.next = toAllocate;

            state = NCAllocate_STORE_PARENT;
            controller.sendStore(parentId, node);
            return false;
        case NCAllocate_STORE_PARENT:
            if(nextNodeId == NODE_ID_INVALID) {
                state = NCAllocate_LOAD;
                controller.sendLoad(toAllocate);
            } else{
                state = NCAllocate_LOAD_RIGHT;
                controller.sendLoad(nextNodeId);
            }
            return false;
        case NCAllocate_LOAD_RIGHT:
            node = pkt->getRaw<Node>();
            node.prev = toAllocate;
            state = NCAllocate_STORE_RIGHT;
            controller.sendStore(nextNodeId, node);
            return false;
        case NCAllocate_STORE_RIGHT:
            state = NCAllocate_LOAD;
            controller.sendLoad(toAllocate);
            return false;
        case NCAllocate_LOAD:
            node = pkt->getRaw<Node>(); // free node
            nextFreeNodeId = node.next;
            node.prev = parentId;
            node.next = nextNodeId;
            if(parentId == NODE_ID_INVALID) {
                controller.tree_root = toAllocate;
            }
            node.state = 1;
            node.counter = 1;
            node.depth = parentDepth + 1;
            controller.sendStore(toAllocate, node);

            state = NCAllocate_STORE;
            
            return false;
        case NCAllocate_STORE:
            if(fromFreeList) {
                controller.free_head = nextFreeNodeId;
            } else{
                ++ controller.freeNodeInited;
            }
            current_pkt->makeResponse();
            current_pkt->deleteData();
            // return a node ID
            current_pkt->setSize(sizeof(NodeID));
            current_pkt->allocate();
            *(current_pkt->getPtr<NodeID>()) = toAllocate;
            // TODO: consider returning status code
            return true;
        default:
            panic("incorrect state for node allocation operation!");
    }
    
}

bool
NodeControllerRevoke::transit(NodeController& controller, PacketPtr current_pkt, PacketPtr pkt) {
    Node node;
    NodeID old_node_id;
    switch(state) {
        case NCRevoke_LOAD_ROOT:
            node = pkt->getRaw<Node>();
            rootDepth = node.depth;
            curNodeId = node.next;
            prevNodeId = node.prev;
            node.state = 0; // invalidate
            if(node.counter == 0){
                // the node can be immediately freed
                controller.freeNode(node, nodeId);
            }
            controller.sendStore(nodeId, node);

            state = NCRevoke_STORE;

            return false;
        case NCRevoke_LOAD:
            node = pkt->getRaw<Node>();
            if(node.depth > rootDepth) {
                // still in the subtree
                node.state = 0;
                old_node_id = curNodeId;
                curNodeId = node.next;
                if(node.counter == 0){
                    // immediately frees the node
                    controller.freeNode(node, old_node_id);
                }
                controller.sendStore(old_node_id, node);

                state = NCRevoke_STORE;
            } else{
                // outside subtree
                // current node is the right node
                // need to update its prevNodeId
                node.prev = prevNodeId;
                controller.sendStore(curNodeId, node);
                state = NCRevoke_STORE_RIGHT;
            }
            return false;
        case NCRevoke_STORE_RIGHT:
            if(prevNodeId == NODE_ID_INVALID) {
                controller.tree_root = curNodeId;
                current_pkt->makeResponse();
                current_pkt->deleteData();
                return true;
            }
            controller.sendLoad(prevNodeId);
            state = NCRevoke_LOAD_LEFT;
            return false;
        case NCRevoke_LOAD_LEFT:
            node = pkt->getRaw<Node>();
            node.next = curNodeId;
            controller.sendStore(prevNodeId, node);
            state = NCRevoke_STORE_LEFT;
            return false;
        case NCRevoke_STORE_LEFT:
            current_pkt->makeResponse();
            current_pkt->deleteData();
            return true;
        case NCRevoke_STORE:
            if(curNodeId == NODE_ID_INVALID) {
                if(prevNodeId == NODE_ID_INVALID) {
                    // the tree is empty
                    controller.tree_root = NODE_ID_INVALID;
                    current_pkt->makeResponse();
                    current_pkt->deleteData();
                    return true;
                }
                // need to change prev->next
                controller.sendLoad(prevNodeId);
                state = NCRevoke_LOAD_LEFT;
            } else{
                controller.sendLoad(curNodeId);
                state = NCRevoke_LOAD;
            }
            return false;
        default:
            panic("incorrect state for node revocation operation!");
    }
}

// when rc reaches 0
// if the node if invalid: add the node to the free list
// if the node is valid: no need to do anything
bool
NodeControllerRcUpdate::transit(NodeController& controller, PacketPtr current_pkt, PacketPtr pkt) {
    Node node;
    switch(state) {
        case NCRcUpdate_LOAD:
            node = pkt->getRaw<Node>();
            node.counter += delta;
            if(node.counter == 0 && node.state == 0) {
                // add node to free list
                controller.freeNode(node, nodeId);
                controller.sendStore(nodeId, node);

                // note that we do not need to do anything 
                // with prev and next because they are invalid notes

                state = NCRcUpdate_STORE;
            } else{
                controller.sendStore(nodeId, node);

                state = NCRcUpdate_STORE;
            }

            return false;
        case NCRcUpdate_STORE:
            current_pkt->makeResponse();
            current_pkt->deleteData();
            // TODO: consider returning status code
            return true;
        default:
            panic("incorrect state for ref count update operation!");
    }
}

void
NodeController::handleResp(PacketPtr pkt) {
    assert(currentPkt != NULL);

    NodeControllerCommand* cmd = currentPkt->getPtr<NodeControllerCommand>();
    assert(cmd != NULL);

    bool finish = cmd->transit(*this, currentPkt, pkt);

    delete pkt;

    if(finish) {
        pkt = currentPkt;
        currentPkt = NULL;

        cpu_side.trySendResp(pkt);
    }
}

Port& NodeController::getPort(const std::string& name, PortID idx) {
    if(name == "cpu_side")
        return cpu_side;
    if(name == "mem_side")
        return mem_side;
    return ClockedObject::getPort(name, idx);
}

NodeController::MemSidePort::MemSidePort(NodeController* owner) :
    RequestPort(owner->name() + ".mem_side", owner),
    owner(owner),
    retryPkt(NULL)
{

}

Addr
NodeController::nodeId2Addr(NodeID node_id) {
    return (Addr)(CAPSTONE_NODE_BASE_ADDR | ((Addr)node_id * (sizeof(Node))));
}

void
NodeController::MemSidePort::trySendReq(PacketPtr pkt) {
    assert(retryPkt == NULL);
    if(!sendTimingReq(pkt)) {
        retryPkt = pkt;
    }
}

void
NodeController::init() {
    ClockedObject::init();

    requestorId = system->getRequestorId(this);
}

void
NodeControllerQuery::setup(NodeController& controller, PacketPtr pkt) {
    DPRINTF(CapstoneNCache, "Read from node cache\n");

    controller.sendLoad(nodeId);
}


void
NodeControllerAllocate::setup(NodeController& controller, PacketPtr pkt) {
    if(controller.free_head == NODE_ID_INVALID) {
        panic_if(controller.freeNodeInited >= CAPSTONE_NODE_N, "no free node remaining.");
        toAllocate = (NodeID)controller.freeNodeInited;
        fromFreeList = false;
    } else{
        toAllocate = controller.free_head;
        fromFreeList = true;
    }
    // TODO: need to handle the case when there are no free nodes
    if(parentId == NODE_ID_INVALID) {
        // skip setting parent
        // same as in NCAllocate_STORE_PARENT
        nextNodeId = controller.tree_root;
        parentDepth = 0;
        if(nextNodeId == NODE_ID_INVALID) {
            state = NCAllocate_LOAD;
            controller.sendLoad(toAllocate);
        } else {
            state = NCAllocate_LOAD_RIGHT;
            controller.sendLoad(nextNodeId);
        }
    } else{
        // load parent first so we know the depth and the next node
        state = NCAllocate_LOAD_PARENT;
        controller.sendLoad(parentId);
    }
}

void
NodeControllerRcUpdate::setup(NodeController& controller, PacketPtr pkt) {
    DPRINTF(CapstoneNodeOps, "rcupdate: %u %d\n", nodeId, delta);
    assert(nodeId != NODE_ID_INVALID);
    assert(delta != 0);
    state = NCRcUpdate_LOAD;
    controller.sendLoad(nodeId);
}

void
NodeControllerRevoke::setup(NodeController& controller, PacketPtr pkt) {
    assert(nodeId != NODE_ID_INVALID);
    state = NCRevoke_LOAD_ROOT;
    controller.sendLoad(nodeId);
}


bool
NodeController::handleReq(PacketPtr pkt) {
    if(currentPkt != NULL) {
        return false;
    }
    
    NodeControllerCommand* cmd = pkt->getPtr<NodeControllerCommand>();
    assert(cmd != NULL);
    cmd->setup(*this, pkt);

    currentPkt = pkt;

    return true;
}


bool NodeController::CPUSidePort::recvTimingReq(PacketPtr pkt) {
    DPRINTF(CapstoneNCache, "NCache packet received\n");
    // no latency
    if(!owner->handleReq(pkt)) {
        toRetryReq = true;
        return false;
    }
    return true;
}


void NodeController::CPUSidePort::recvRespRetry() {
    assert(retryPkt != NULL);
    PacketPtr pkt = retryPkt;
    retryPkt = NULL;
    trySendResp(pkt);
}

void NodeController::CPUSidePort::trySendResp(PacketPtr pkt) {
    DPRINTF(CapstoneNCache, "NCacheController try sending response\n");
    assert(retryPkt == NULL);
    if(!sendTimingResp(pkt)) {
        retryPkt = pkt;
    } else if(toRetryReq){
        toRetryReq = false;
        sendRetryReq(); // ready to receive new request
    }
}


void NodeController::CPUSidePort::recvFunctional(PacketPtr pkt) {
    recvTimingReq(pkt);
}


AddrRangeList NodeController::CPUSidePort::getAddrRanges() const {
    return std::list<AddrRange> { AddrRange(0, 0xffffffffLL) };
}
    
void
NodeController::allocObject(const AddrRange& obj) {
    objectRanges.push_back(obj);
}

void
NodeController::freeObject(Addr addr) {
    objectRanges.remove_if([addr](auto obj) { return obj.contains(addr); });
}

std::optional<int>
NodeController::lookupAddr(Addr addr) {
    int n = 0;
    for(auto& obj : objectRanges) {
        if(obj.contains(addr)){
            return std::optional<int>(n);
        }
        ++ n;
    }
    return std::optional<int>();
}

void
NodeController::regStats() {
    ClockedObject::regStats();
}

void
NodeController::addCapTrack(const CapLoc& loc, NodeID node_id) {
    DPRINTF(CapstoneCapTrack, "cap track added with node %u, %s\n", node_id,
            loc.toString().c_str());
    capTrackMap[loc] = node_id;
}

NodeID
NodeController::queryCapTrack(const CapLoc& loc) {
    //DPRINTF(CapstoneCapTrack, "cap track queried\n");
    try{
        return capTrackMap.at(loc);
    } catch(const std::out_of_range& e) {
        return NODE_ID_INVALID;
    }
}

void
NodeController::removeCapTrack(const CapLoc& loc) {
    DPRINTF(CapstoneCapTrack, "cap track removed %s\n",
            loc.toString().c_str());
    capTrackMap.erase(loc);
}

void
NodeController::freeNode(Node& node, NodeID node_id) {
    DPRINTF(CapstoneNodeOps, "free node with id %lu\n", node_id);
    node.next = free_head;
    free_head = node_id;
}


} // end of namespace gem5::RiscvcapstoneISA

