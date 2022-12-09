#include "cpu/thread_context.hh"
#include "arch/riscvcapstone/o3/ncq_unit.hh"
#include "arch/riscvcapstone/o3/dyn_inst.hh"
#include "arch/riscvcapstone/o3/ncq.hh"
#include "arch/riscvcapstone/o3/lsq.hh"


namespace gem5 {
namespace RiscvcapstoneISA {
namespace o3 {


NCQUnit::NCQUnit(ThreadID thread_id, int queue_size,
        NCQ* ncq) :
    threadId(thread_id),
    ncQueue(queue_size),
    queueSize(queue_size),
    ncq(ncq)
{
}

void
NCQUnit::insertInstruction(const DynInstPtr& inst) {
    assert(!ncQueue.full());
    ncQueue.advance_tail();
    ncQueue.back() = NCQEntry(inst);
    assert(!ncQueue.empty());

    inst->ncqIdx = ncQueue.tail();
    inst->ncqIt = ncQueue.end() - 1;
}

void
NCQUnit::tick() {
    // for testing
    if(!ncQueue.empty())
        ncQueue.pop_front();
    assert(!ncQueue.full());
}

Fault
NCQUnit::pushCommand(const DynInstPtr& inst, NodeCommandPtr cmd) {
    assert(inst->ncqIdx != -1); // inst has been inserted to this queue
    NCQEntry& ncq_entry = *(inst->ncqIt);
    assert(ncq_entry.inst->seqNum == inst->seqNum); // indeed the same inst in the entry

    ncq_entry.commands.push_back(cmd);

    return NoFault;
}

bool
NCQUnit::isFull() {
    return ncQueue.full();
}

void
NCQUnit::commitBefore(InstSeqNum seq_num) {
    for(NCQIterator it = ncQueue.begin(); 
            it != ncQueue.end() && it->inst->seqNum <= seq_num;
            ++ it) {
        it->canWB = true;
    }
}

void
NCQUnit::writebackCommands(){
    // not doing lots of reordering right now
    for(NCQIterator it = ncQueue.begin();
            it != ncQueue.end() && ncq->canSend(); ++ it) {
        if(!it->inst->isExecuted() || it->completed())
            // not doing anything for instructions not yet executed
            continue;
        std::vector<NodeCommandPtr>& commands = it->commands;
        for(NodeCommandIterator nc_it = commands.begin();
                nc_it != commands.end() && ncq->canSend();
                ++ nc_it) {
            NodeCommandPtr nc_ptr = *nc_it;
            if(nc_ptr->status == NodeCommand::COMPLETED || 
                nc_ptr->status == NodeCommand::AWAIT_CACHE ||
                (!nc_ptr->beforeCommit() && !it->canWB))
                continue;

            if(nc_ptr->status == NodeCommand::NOT_STARTED) {
                auto& cond_ptr = nc_ptr->condition;
                auto saved_req = it->inst->savedRequest;
                if(cond_ptr && saved_req &&
                        (!saved_req->isComplete() || !cond_ptr->satisfied(saved_req))){
                    // cannot process if the request has not completed or 
                    // the condition is not satisfied
                    continue;
                }
            }

            // check for dependencies
            // the naive way. Bruteforce
            bool dep_ready = true;
            for(NCQIterator it_o = ncQueue.begin();
                    dep_ready;
                    ++ it_o) {
                for(NodeCommandIterator nc_it_o; 
                        nc_it_o != it_o->commands.end() && (it_o != it ||
                            nc_it_o != nc_it); 
                        ++ nc_it_o) {
                    NodeCommandPtr nc_ptr_o = *nc_it_o;
                    if(nc_ptr_o->status != NodeCommand::COMPLETED && 
                            !ncOrder.reorderAllowed(nc_ptr_o, nc_ptr)){
                        dep_ready = false;
                        break;
                    }
                }
                if(it_o == it)
                    break;
            }
            
            if(!dep_ready)
                continue;

            // the command can be executed
            // one state transition in the state machine
            PacketPtr pkt = nc_ptr->transition();
            if(pkt) {
                ncq->trySendPacket(pkt, threadId);
                // record which command the packet originates from
                // to deliver the packet back once the response if received
                assert(packetIssuers.find(pkt->id) == packetIssuers.end());
                packetIssuers[pkt->id] = nc_ptr;
            } else if(nc_ptr->status == NodeCommand::COMPLETED) {
                completeCommand(nc_ptr);
            }
        }

    }
}


void
NCQUnit::completeCommand(NodeCommandPtr node_command){
    node_command->inst->completeNodeAcc(node_command);
    ++ node_command->inst->ncqIt->completedCommands;
}

bool
NCQUnit::handleCacheResp(PacketPtr pkt) {
    auto it = packetIssuers.find(pkt->id);
    assert(it != packetIssuers.end());
    NodeCommandPtr node_cmd = it->second;
    assert(node_cmd);
    packetIssuers.erase(it);
    node_cmd->handleResp(pkt);
    if(node_cmd->status == NodeCommand::COMPLETED) {
        completeCommand(node_cmd);
    }

    return true;
}


}
}
}


