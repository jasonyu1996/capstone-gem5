from m5 import *
from m5.proxy import *
from m5.objects.ClockedObject import ClockedObject


class NodeController(ClockedObject):
    type = 'NodeController'
    cxx_header = 'arch/riscvcapstone/node_controller.hh'
    cxx_class = 'gem5::RiscvcapstoneISA::NodeController'

    cpu_side = ResponsePort('port typically connected to the CPU dcache port')
    # mem_side = RequestPort('memory-side port')

