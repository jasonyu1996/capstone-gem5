#ifndef __CAPSTONE_TAG_CONTROLLER_H__
#define __CAPSTONE_TAG_CONTROLLER_H__

#include<type_traits>
#include<unordered_set>
#include<unordered_map> 
#include<list>
#include<vector>
#include "base/types.hh"
#include "base/circular_queue.hh"
#include "mem/port.hh"
#include "arch/riscvcapstone/o3/dyn_inst_ptr.hh"
#include "arch/riscvcapstone/o3/node.hh"

namespace gem5 {
namespace RiscvcapstoneISA {
namespace o3 {

class CPU;
class IEW;

class BaseTagController {
    protected:
        struct TagOp { // tag changes
            Addr addr;
            bool tagSet;
        };

        struct TagEntry {
            DynInstPtr inst;
            std::list<TagOp> ops;
            bool canWB;
        };

        //using TagQueue = CircularQueue<TagEntry>;
        using TagQueue = CircularQueue<TagEntry>;

        int threadCount, queueSize;

        std::unordered_set<Addr> taggedAddrs; // only the committed tags
        std::vector<TagQueue> tagQueues; // uncommitted tags

        static bool aligned(Addr addr) {
            return true;
        }


        BaseTagController(int thread_count, int queue_size);

        virtual bool writebackTagEntry(TagEntry& tag_entry);
        virtual bool writebackTagOp(DynInstPtr& inst, TagOp& tag_op) = 0;
    
    public:
        using TQIterator = typename TagQueue::iterator;

        bool getTag(const DynInstPtr& inst, Addr addr, bool& delayed);
        virtual bool getCommittedTag(const DynInstPtr& inst, Addr addr, bool& delayed) = 0;
        void setTag(const DynInstPtr& inst, Addr addr, bool tag);
        /**
         * Commit instructions before the given sequence number
         * */
        void commitBefore(InstSeqNum seq_num, ThreadID thread_id);
        virtual void tick() = 0;
        virtual void writeback();
        // insert instruction during dispatch (in-order)
        virtual void insertInstruction(const DynInstPtr& inst);

        bool isFull(ThreadID thread_id) {
            assert(thread_id >= 0 && thread_id < threadCount);
            return tagQueues[thread_id].full();
        }
};

class MockTagController : public BaseTagController {
    private:
        static const int REG_N = 32;
        using RegTagMap = std::vector<bool>;
        std::vector<RegTagMap> regTagMaps;
    protected:
        bool writebackTagOp(DynInstPtr& inst, TagOp& tag_op) override;
    public:
        MockTagController(int thread_count, int queue_size);
        MockTagController(const MockTagController& other) = delete;
        bool getCommittedTag(const DynInstPtr& inst, Addr addr, bool& delayed) override;

        void tick() override {}

        bool getRegTag(RegIndex reg_idx, ThreadID thread_id) const;
        void setRegTag(RegIndex reg_idx, bool tag, ThreadID thread_id);

        void allocObject(const SimpleAddrRange& range) { }
        void freeObject(Addr addr) { }
};

using TagController = MockTagController;

}
}
}


#endif
