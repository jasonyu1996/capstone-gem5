#include "cpu/thread_context.hh"
#include "arch/riscvcapstone/o3/ncq_unit.hh"
#include "arch/riscvcapstone/o3/dyn_inst.hh"
#include "arch/riscvcapstone/o3/ncq.hh"
#include "arch/riscvcapstone/o3/lsq.hh"
#include "arch/riscvcapstone/o3/iew.hh"
#include "arch/riscvcapstone/o3/cpu.hh"
#include "debug/NCQ.hh"


namespace gem5 {
namespace RiscvcapstoneISA {
namespace o3 {

class CPU;

NCQUnit::NCQUnit(ThreadID thread_id, int queue_size,
        CPU* cpu, NCQ* ncq, IEW* iew) :
    threadId(thread_id),
    ncQueue(queue_size),
    queueSize(queue_size),
    cpu(cpu),
    ncq(ncq), iew(iew)
{
}

void
NCQUnit::insertInstruction(const DynInstPtr& inst) {
    assert(!ncQueue.full());
    ncQueue.advance_tail();
    ncQueue.back() = NCQEntry(inst);

    inst->ncqIdx = ncQueue.tail();
    inst->ncqIt = ncQueue.end() - 1;

    assert(!ncQueue.empty());
    assert(ncQueue.size() != 0);
    DPRINTF(NCQ, "Pushed instruction %u to %u of NCQ thread %u\n",
            inst->seqNum, inst->ncqIdx, threadId);
}

void
NCQUnit::tick() {
}

void
NCQUnit::dumpNcQueue() {
    DPRINTF(NCQ, "Dumping the NCQ\n");
    for(size_t i = ncQueue.head(); i <= ncQueue.tail(); i++) {
        if(ncQueue[i].inst)
            DPRINTF(NCQ, "Instruction = %u\n", ncQueue[i].inst->seqNum);
        else
            DPRINTF(NCQ, "Instruction = %u from commit\n", ncQueue[i].seqNum);
        for(auto &nc: ncQueue[i].commands) {
            DPRINTF(NCQ, "Command = %u, beforeCommit = %u, command status = %u\n",
                    nc->getType(), nc->beforeCommit(), static_cast<unsigned int>(nc->status));
        }
    }
}

Fault
NCQUnit::pushCommand(const DynInstPtr& inst, NodeCommandPtr cmd) {
    assert(inst->ncqIdx != -1); // inst has been inserted to this queue
    NCQEntry& ncq_entry = *(inst->ncqIt);
    assert(ncq_entry.inst->seqNum == inst->seqNum); // indeed the same inst in the entry

    ncq_entry.commands.push_back(cmd);

    dumpNcQueue();

    return NoFault;
}

Fault
NCQUnit::pushCommand(NodeCommandPtr cmd) {
    assert(!ncQueue.full());
    ncQueue.advance_tail();
    ncQueue.back() = NCQEntry(nullptr);

    NCQEntry& ncq_entry = *(ncQueue.end() - 1);

    cmd->dump();

    ncq_entry.commands.push_back(cmd);
    ncq_entry.seqNum = cmd->seqNum;
    cmd->ncq_ptr = &ncq_entry;

    dumpNcQueue();

    return NoFault;
}

bool
NCQUnit::isFull() {
    dumpNcQueue();
    return ncQueue.full();
}

void
NCQUnit::commitBefore(InstSeqNum seq_num) {
    DPRINTF(NCQ, "Committing instructions before %u in thread %u NCQ" 
            " (containing %u instructions)\n",
            seq_num, threadId, ncQueue.size());
    for(NCQIterator it = ncQueue.begin(); 
            it != ncQueue.end();
            ++ it) {
                // continue or break 
        if(it->inst && it->inst->seqNum > seq_num)
            break;
        DPRINTF(NCQ, "Marking commands as canWB\n");
        it->canWB = true;
    }
}

void
NCQUnit::cleanupCommands(){
    DPRINTF(NCQ, "Cleaning up commands\n");
    while(!ncQueue.empty()) {
        auto& front = ncQueue.front();
        if(front.inst)
            DPRINTF(NCQ, "cleanupCommands: inst %u, canWB %u, completed() %u, commands size() %u", front.inst->seqNum, front.canWB, front.completed(), front.commands.size());
        else
            DPRINTF(NCQ, "cleanupCommands: inst %u, canWB %u, completed() %u, commands size() %u from commit", front.seqNum, front.canWB, front.completed(), front.commands.size());
        if(front.canWB && front.completed()) {
            if(front.inst) {
                DPRINTF(NCQ, "Removing NCQEntry for instruction %u\n", front.inst->seqNum);
                front.inst->ncqIdx = -1;
            }
            else
                DPRINTF(NCQ, "Removing NCQEntry for instruction %u from commit\n", front.seqNum);
            front.clear();
            ncQueue.pop_front();
        } else{
            break;
        }
    }
}

void
NCQUnit::writebackCommands(){
    // not doing lots of reordering right now
    for(NCQIterator it = ncQueue.begin();
            it != ncQueue.end() && ncq->canSend(); ++ it) {
        //if(!it->inst->isNodeInitiated() || it->completed())
        if(it->completed())
            // not doing anything for instructions not yet executed
            continue;
        std::vector<NodeCommandPtr>& commands = it->commands;
        /** @todo maybe insert a seqNum only with commit commands */
        if(it->inst)
            DPRINTF(NCQ, "Instruction %u with %u commands (completed = %u)\n", it->inst->seqNum, commands.size(), 
                    it->completedCommands);
        else
            DPRINTF(NCQ, "Instruction = %u. Command from commit.\n", it->seqNum);
        for(NodeCommandIterator nc_it = commands.begin();
                nc_it != commands.end() && ncq->canSend();
                ++ nc_it) {
            NodeCommandPtr nc_ptr = *nc_it;
            assert(nc_ptr);
            DPRINTF(NCQ, "Command = %u\n", nc_ptr->getType());
            DPRINTF(NCQ, "Command status = %u, before commit = %u\n",
                    static_cast<unsigned int>(nc_ptr->status),
                    nc_ptr->beforeCommit());
            if(nc_ptr->status == NodeCommand::COMPLETED || 
                nc_ptr->status == NodeCommand::AWAIT_CACHE)
                //(!nc_ptr->beforeCommit() && !it->canWB))
                continue;

            // if(nc_ptr->status == NodeCommand::NOT_STARTED) {
            //     auto& cond_ptr = nc_ptr->condition;
            //     auto saved_req = it->inst->savedRequest;
            //     if(cond_ptr && saved_req &&
            //             (!saved_req->isComplete() || !cond_ptr->satisfied(saved_req))){
            //         DPRINTF(NCQ, "Command bypassed as condition is not satisfied\n");
            //         // cannot process if the request has not completed or 
            //         // the condition is not satisfied
            //         continue;
            //     }
            // }

            DPRINTF(NCQ, "Checking command dependencies\n");

            InstSeqNum sn, sn_o;

            if(it->inst)
                sn = it->inst->seqNum;
            else
                sn = it->seqNum;

            // check for dependencies
            // the naive way. Bruteforce
            bool dep_ready = true;
            for(NCQIterator it_o = ncQueue.begin();
                    dep_ready && it_o != ncQueue.end();
                    ++ it_o) {
                for(NodeCommandIterator nc_it_o = it_o->commands.begin(); 
                        nc_it_o != it_o->commands.end() && (it_o != it ||
                            nc_it_o != nc_it); 
                        ++ nc_it_o) {
                    NodeCommandPtr nc_ptr_o = *nc_it_o;
                    assert(nc_ptr_o);
                    if(it_o->inst)
                        sn_o = it_o->inst->seqNum;
                    else
                        sn_o = it_o->seqNum;
                    /** compare seqNum because commands from commit may be issued
                      * after commands from execute.
                      * there might be a better way to do it though
                      */
                    if(nc_ptr_o->status != NodeCommand::COMPLETED && sn_o < sn &&
                            !ncOrder.reorderAllowed(nc_ptr_o, nc_ptr)){
                        dep_ready = false;
                        break;
                    }
                }
                // if(it_o == it)
                    // break;
            }
            
            if(!dep_ready)
                continue;

            if(it->inst)
                DPRINTF(NCQ, "Command ready to execute (instruction %u)\n",
                        it->inst->seqNum);
            else
                DPRINTF(NCQ, "Command ready to execute (instruction %u)\n",
                        it->seqNum);

            // the command can be executed
            // one state transition in the state machine
            PacketPtr pkt = nc_ptr->transition();
            if(pkt) {
                ncq->trySendPacket(pkt, threadId);
                DPRINTF(NCQ, "Packet sent for command\n");
                // record which command the packet originates from
                // to deliver the packet back once the response if received
                assert(packetIssuers.find(pkt->id) == packetIssuers.end());
                packetIssuers[pkt->id] = PacketRecord {
                    .inst = it->inst, 
                    .cmd = nc_ptr
                };
            } else if(nc_ptr->status == NodeCommand::COMPLETED) {
                completeCommand(nc_ptr);
            }
        }

    }
}

void
NCQUnit::completeCommand(NodeCommandPtr node_command){
    DynInstPtr& inst = node_command->inst;
    if(inst) {
        DPRINTF(NCQ, "Command for instruction %u completed\n", inst->seqNum);
        Fault fault = inst->completeNodeAcc(node_command);
        if(fault != NoFault) {
            cpu->trap(fault, threadId, inst->staticInst); // FIXME: should be passed to commit instead
        }
        ++ inst->ncqIt->completedCommands;
        if(inst->ncqIt->completed() &&
                inst->hasNodeWB()) {
            DPRINTF(NCQ, "Instruction %u can now be committed\n",
                    inst->seqNum);
            inst->setNodeExecuted();
            iew->instToCommitIfExeced(inst);
        }
    } else {
        ++node_command->ncq_ptr->completedCommands;
    }
}

bool
NCQUnit::handleCacheResp(PacketPtr pkt) {
    auto it = packetIssuers.find(pkt->id);
    assert(it != packetIssuers.end());
    PacketRecord& packet_record = it->second;
    NodeCommandPtr node_cmd = packet_record.cmd;
    if(packet_record.inst) {
        DPRINTF(NCQ, "Node cache response received for instruction %u, cmd beforeCommit = %u\n",
                        packet_record.inst->seqNum, node_cmd->beforeCommit());

        if(packet_record.inst->ncqIdx < 0) {
            packetIssuers.erase(it);
            delete pkt;
            return true;
        }
    }
    packetIssuers.erase(it);
    node_cmd->handleResp(pkt); // node_cmd is expected to handle the freeing of pkt
    DPRINTF(NCQ, "Command handler new status = %u\n", static_cast<unsigned int>(node_cmd->status));
    if(node_cmd->status == NodeCommand::COMPLETED) {
        DPRINTF(NCQ ,"Completed node command (type = %d)\n", (int)node_cmd->getType());
        completeCommand(node_cmd);
    }

    return true;
}

QueryResult
NCQUnit::passedQuery(const DynInstPtr& inst) const {
    assert(inst->threadNumber == threadId);
    if(!inst->hasNodeOp()) // no associated command
        return QueryResult::PASSED;
    assert(inst->ncqIdx != -1);
    auto& commands = inst->ncqIt->commands;
    for(auto it = commands.begin();
            it != commands.end();
            ++ it) {
        NodeCommandPtr& node_command = *it;
        if(node_command->beforeCommit()) {
            if(node_command->status != NodeCommand::COMPLETED) {
                return QueryResult::PENDING;
            } else if(node_command->error()) {
                return QueryResult::FAILED;
            }
        }
    }
    return QueryResult::PASSED;
}

void
NCQUnit::squash(const InstSeqNum &squashed_num) {
    if(!ncQueue.empty()) {
        DPRINTF(NCQ, "Squashing till seqNum = %u. NcQueue so far:\n", squashed_num);
        dumpNcQueue();
        NCQIterator temp = ncQueue.end() - 1;
    // while(!ncQueue.empty()) {
    //     if(ncQueue.back().inst && ncQueue.back().inst->seqNum > squashed_num) {
    //         NCQEntry& back = ncQueue.back();
    //         back.inst->setSquashed();
    //         back.inst->ncqIdx = -1;
    //         back.clear();
    //         ncQueue.pop_back();
    //     }
    // }
        for( ;!ncQueue.empty() && temp != ncQueue.begin(); temp--) {
            NCQEntry &ncq = *temp;
            if(ncq.inst && ncq.inst->seqNum > squashed_num) {
                DPRINTF(NCQ, "Squashing NCQ entry for seqNum = %u\n", ncq.inst->seqNum);
                ncq.inst->setSquashed();
                ncq.inst->ncqIdx = -1;
                ncq.clear();
                ncQueue.pop_i(temp.idx());
            }
        }
    }
}


}
}
}


