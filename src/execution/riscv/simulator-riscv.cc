// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/riscv/simulator-riscv.h"

// Only build the simulator if not compiling for real RISCV hardware.
#if defined(USE_SIMULATOR)

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <cfenv>
#include <cmath>

#include "src/base/bits.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/riscv/constants-riscv.h"
#include "src/diagnostics/disasm.h"
#include "src/heap/combined-heap.h"
#include "src/runtime/runtime-utils.h"
#include "src/utils/ostreams.h"
#include "src/utils/vector.h"

namespace v8 {
namespace internal {

DEFINE_LAZY_LEAKY_OBJECT_GETTER(Simulator::GlobalMonitor,
                                Simulator::GlobalMonitor::Get)

// Util functions.
inline bool HaveSameSign(int64_t a, int64_t b) { return ((a ^ b) >= 0); }

uint32_t get_fcsr_condition_bit(uint32_t cc) {
  if (cc == 0) {
    return 23;
  } else {
    return 24 + cc;
  }
}
// FIXME(RISCV) This function is not used in RISCV.  commented it out. RY
/*
static int64_t MultiplyHighSigned(int64_t u, int64_t v) {
  uint64_t u0, a0, w0;
  int64_t u1, a1, w1, w2, t;

  u0 = u & 0xFFFFFFFFL;
  u1 = u >> 32;
  a0 = v & 0xFFFFFFFFL;
  a1 = v >> 32;

  w0 = u0 * a0;
  t = u1 * a0 + (w0 >> 32);
  w1 = t & 0xFFFFFFFFL;
  w2 = t >> 32;
  w1 = u0 * a1 + w1;

  return u1 * a1 + w2 + (w1 >> 32);
}
*/
// This macro provides a platform independent use of sscanf. The reason for
// SScanF not being implemented in a platform independent was through
// ::v8::internal::OS in the same way as SNPrintF is that the Windows C Run-Time
// Library does not provide vsscanf.
#define SScanF sscanf  // NOLINT

// The RiscvDebugger class is used by the simulator while debugging simulated
// code.
class RiscvDebugger {
 public:
  explicit RiscvDebugger(Simulator* sim) : sim_(sim) {}

  void Stop(Instruction* instr);
  void Debug();
  // Print all registers with a nice formatting.
  void PrintAllRegs();
  void PrintAllRegsIncludingFPU();

 private:
  // FIXME (RISCV): SPECIAL and BRREAK are MIPS constants
  // We set the breakpoint code to 0xFFFFF to easily recognize it.
  static const Instr kBreakpointInstr = SPECIAL | BREAK | 0xFFFFF << 6;
  static const Instr kNopInstr = 0x0;

  Simulator* sim_;

  int64_t GetRegisterValue(int regnum);
  int64_t GetFPURegisterValue(int regnum);
  float GetFPURegisterValueFloat(int regnum);
  double GetFPURegisterValueDouble(int regnum);
  bool GetValue(const char* desc, int64_t* value);

  // Set or delete a breakpoint. Returns true if successful.
  bool SetBreakpoint(Instruction* breakpc);
  bool DeleteBreakpoint(Instruction* breakpc);

  // Undo and redo all breakpoints. This is needed to bracket disassembly and
  // execution to skip past breakpoints when run from the debugger.
  void UndoBreakpoints();
  void RedoBreakpoints();
};

inline void UNSUPPORTED() {
  printf("Sim: Unsupported instruction.\n");
  base::OS::Abort();
}

void RiscvDebugger::Stop(Instruction* instr) {
  // Get the stop code.
  uint32_t code = instr->Bits(25, 6);
  PrintF("Simulator hit (%u)\n", code);
  Debug();
}

int64_t RiscvDebugger::GetRegisterValue(int regnum) {
  if (regnum == kNumSimuRegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_register(regnum);
  }
}

int64_t RiscvDebugger::GetFPURegisterValue(int regnum) {
  if (regnum == kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_fpu_register(regnum);
  }
}

float RiscvDebugger::GetFPURegisterValueFloat(int regnum) {
  if (regnum == kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_fpu_register_float(regnum);
  }
}

double RiscvDebugger::GetFPURegisterValueDouble(int regnum) {
  if (regnum == kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_fpu_register_double(regnum);
  }
}

bool RiscvDebugger::GetValue(const char* desc, int64_t* value) {
  int regnum = Registers::Number(desc);
  int fpuregnum = FPURegisters::Number(desc);

  if (regnum != kInvalidRegister) {
    *value = GetRegisterValue(regnum);
    return true;
  } else if (fpuregnum != kInvalidFPURegister) {
    *value = GetFPURegisterValue(fpuregnum);
    return true;
  } else if (strncmp(desc, "0x", 2) == 0) {
    return SScanF(desc + 2, "%" SCNx64, reinterpret_cast<uint64_t*>(value)) ==
           1;
  } else {
    return SScanF(desc, "%" SCNu64, reinterpret_cast<uint64_t*>(value)) == 1;
  }
  return false;
}

bool RiscvDebugger::SetBreakpoint(Instruction* breakpc) {
  // Check if a breakpoint can be set. If not return without any side-effects.
  if (sim_->break_pc_ != nullptr) {
    return false;
  }

  // Set the breakpoint.
  sim_->break_pc_ = breakpc;
  sim_->break_instr_ = breakpc->InstructionBits();
  // Not setting the breakpoint instruction in the code itself. It will be set
  // when the debugger shell continues.
  return true;
}

bool RiscvDebugger::DeleteBreakpoint(Instruction* breakpc) {
  if (sim_->break_pc_ != nullptr) {
    sim_->break_pc_->SetInstructionBits(sim_->break_instr_);
  }

  sim_->break_pc_ = nullptr;
  sim_->break_instr_ = 0;
  return true;
}

void RiscvDebugger::UndoBreakpoints() {
  if (sim_->break_pc_ != nullptr) {
    sim_->break_pc_->SetInstructionBits(sim_->break_instr_);
  }
}

void RiscvDebugger::RedoBreakpoints() {
  if (sim_->break_pc_ != nullptr) {
    sim_->break_pc_->SetInstructionBits(kBreakpointInstr);
  }
}

void RiscvDebugger::PrintAllRegs() {
#define REG_INFO(n) Registers::Name(n), GetRegisterValue(n), GetRegisterValue(n)

  PrintF("\n");
  // at, a0, a0.
  PrintF("%3s: 0x%016" PRIx64 " %14" PRId64 "\t%3s: 0x%016" PRIx64 " %14" PRId64
         "\t%3s: 0x%016" PRIx64 " %14" PRId64 "\n",
         REG_INFO(1), REG_INFO(2), REG_INFO(4));
  // a1, a1.
  PrintF("%34s\t%3s: 0x%016" PRIx64 "  %14" PRId64 " \t%3s: 0x%016" PRIx64
         "  %14" PRId64 " \n",
         "", REG_INFO(3), REG_INFO(5));
  // a2.
  PrintF("%34s\t%34s\t%3s: 0x%016" PRIx64 "  %14" PRId64 " \n", "", "",
         REG_INFO(6));
  // a3.
  PrintF("%34s\t%34s\t%3s: 0x%016" PRIx64 "  %14" PRId64 " \n", "", "",
         REG_INFO(7));
  PrintF("\n");
  // a4-t3, s0-s7
  for (int i = 0; i < 8; i++) {
    PrintF("%3s: 0x%016" PRIx64 "  %14" PRId64 " \t%3s: 0x%016" PRIx64
           "  %14" PRId64 " \n",
           REG_INFO(8 + i), REG_INFO(16 + i));
  }
  PrintF("\n");
  // t8, k0, LO.
  PrintF("%3s: 0x%016" PRIx64 "  %14" PRId64 " \t%3s: 0x%016" PRIx64
         "  %14" PRId64 " \t%3s: 0x%016" PRIx64 "  %14" PRId64 " \n",
         REG_INFO(24), REG_INFO(26), REG_INFO(32));
  // t9, k1, HI.
  PrintF("%3s: 0x%016" PRIx64 "  %14" PRId64 " \t%3s: 0x%016" PRIx64
         "  %14" PRId64 " \t%3s: 0x%016" PRIx64 "  %14" PRId64 " \n",
         REG_INFO(25), REG_INFO(27), REG_INFO(33));
  // sp, fp, gp.
  PrintF("%3s: 0x%016" PRIx64 "  %14" PRId64 " \t%3s: 0x%016" PRIx64
         "  %14" PRId64 " \t%3s: 0x%016" PRIx64 "  %14" PRId64 " \n",
         REG_INFO(29), REG_INFO(30), REG_INFO(28));
  // pc.
  PrintF("%3s: 0x%016" PRIx64 "  %14" PRId64 " \t%3s: 0x%016" PRIx64
         "  %14" PRId64 " \n",
         REG_INFO(31), REG_INFO(34));

#undef REG_INFO
}

void RiscvDebugger::PrintAllRegsIncludingFPU() {
#define FPU_REG_INFO(n) \
  FPURegisters::Name(n), GetFPURegisterValue(n), GetFPURegisterValueDouble(n)

  PrintAllRegs();

  PrintF("\n\n");
  // f0, f1, f2, ... f31.
  // TODO(plind): consider printing 2 columns for space efficiency.
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(0));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(1));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(2));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(3));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(4));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(5));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(6));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(7));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(8));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(9));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(10));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(11));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(12));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(13));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(14));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(15));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(16));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(17));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(18));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(19));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(20));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(21));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(22));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(23));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(24));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(25));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(26));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(27));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(28));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(29));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(30));
  PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n", FPU_REG_INFO(31));

#undef FPU_REG_INFO
}

// FIXME (RISCV): to be ported
void RiscvDebugger::Debug() {
  UNIMPLEMENTED();

  /*
    intptr_t last_pc = -1;
    bool done = false;

  #define COMMAND_SIZE 63
  #define ARG_SIZE 255

  #define STR(a) #a
  #define XSTR(a) STR(a)

    char cmd[COMMAND_SIZE + 1];
    char arg1[ARG_SIZE + 1];
    char arg2[ARG_SIZE + 1];
    char* argv[3] = {cmd, arg1, arg2};

    // Make sure to have a proper terminating character if reaching the limit.
    cmd[COMMAND_SIZE] = 0;
    arg1[ARG_SIZE] = 0;
    arg2[ARG_SIZE] = 0;

    // Undo all set breakpoints while running in the debugger shell. This will
    // make them invisible to all commands.
    UndoBreakpoints();

    while (!done && (sim_->get_pc() != Simulator::end_sim_pc)) {
      if (last_pc != sim_->get_pc()) {
        disasm::NameConverter converter;
        disasm::Disassembler dasm(converter);
        // Use a reasonably large buffer.
        v8::internal::EmbeddedVector<char, 256> buffer;
        dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(sim_->get_pc()));
        PrintF("  0x%016" PRIx64 "   %s\n", sim_->get_pc(), buffer.begin());
        last_pc = sim_->get_pc();
      }
      char* line = ReadLine("sim> ");
      if (line == nullptr) {
        break;
      } else {
        char* last_input = sim_->last_debugger_input();
        if (strcmp(line, "\n") == 0 && last_input != nullptr) {
          line = last_input;
        } else {
          // Ownership is transferred to sim_;
          sim_->set_last_debugger_input(line);
        }
        // Use sscanf to parse the individual parts of the command line. At the
        // moment no command expects more than two parameters.
        int argc = SScanF(
            line,
            "%" XSTR(COMMAND_SIZE) "s "
            "%" XSTR(ARG_SIZE) "s "
            "%" XSTR(ARG_SIZE) "s",
            cmd, arg1, arg2);
        if ((strcmp(cmd, "si") == 0) || (strcmp(cmd, "stepi") == 0)) {
          Instruction* instr = reinterpret_cast<Instruction*>(sim_->get_pc());
          if (!(instr->IsTrap()) ||
              instr->InstructionBits() == rtCallRedirInstr) {
            sim_->InstructionDecode(
                reinterpret_cast<Instruction*>(sim_->get_pc()));
          } else {
            // Allow si to jump over generated breakpoints.
            PrintF("/!\\ Jumping over generated breakpoint.\n");
            sim_->set_pc(sim_->get_pc() + kInstrSize);
          }
        } else if ((strcmp(cmd, "c") == 0) || (strcmp(cmd, "cont") == 0)) {
          // Execute the one instruction we broke at with breakpoints disabled.
          sim_->InstructionDecode(reinterpret_cast<Instruction*>(sim_->get_pc()));
          // Leave the debugger shell.
          done = true;
        } else if ((strcmp(cmd, "p") == 0) || (strcmp(cmd, "print") == 0)) {
          if (argc == 2) {
            int64_t value;
            double dvalue;
            if (strcmp(arg1, "all") == 0) {
              PrintAllRegs();
            } else if (strcmp(arg1, "allf") == 0) {
              PrintAllRegsIncludingFPU();
            } else {
              int regnum = Registers::Number(arg1);
              int fpuregnum = FPURegisters::Number(arg1);

              if (regnum != kInvalidRegister) {
                value = GetRegisterValue(regnum);
                PrintF("%s: 0x%08" PRIx64 "  %" PRId64 "  \n", arg1, value,
                       value);
              } else if (fpuregnum != kInvalidFPURegister) {
                value = GetFPURegisterValue(fpuregnum);
                dvalue = GetFPURegisterValueDouble(fpuregnum);
                PrintF("%3s: 0x%016" PRIx64 "  %16.4e\n",
                       FPURegisters::Name(fpuregnum), value, dvalue);
              } else {
                PrintF("%s unrecognized\n", arg1);
              }
            }
          } else {
            if (argc == 3) {
              if (strcmp(arg2, "single") == 0) {
                int64_t value;
                float fvalue;
                int fpuregnum = FPURegisters::Number(arg1);

                if (fpuregnum != kInvalidFPURegister) {
                  value = GetFPURegisterValue(fpuregnum);
                  value &= 0xFFFFFFFFUL;
                  fvalue = GetFPURegisterValueFloat(fpuregnum);
                  PrintF("%s: 0x%08" PRIx64 "  %11.4e\n", arg1, value, fvalue);
                } else {
                  PrintF("%s unrecognized\n", arg1);
                }
              } else {
                PrintF("print <fpu register> single\n");
              }
            } else {
              PrintF("print <register> or print <fpu register> single\n");
            }
          }
        } else if ((strcmp(cmd, "po") == 0) ||
                   (strcmp(cmd, "printobject") == 0)) {
          if (argc == 2) {
            int64_t value;
            StdoutStream os;
            if (GetValue(arg1, &value)) {
              Object obj(value);
              os << arg1 << ": \n";
  #ifdef DEBUG
              obj.Print(os);
              os << "\n";
  #else
              os << Brief(obj) << "\n";
  #endif
            } else {
              os << arg1 << " unrecognized\n";
            }
          } else {
            PrintF("printobject <value>\n");
          }
        } else if (strcmp(cmd, "stack") == 0 || strcmp(cmd, "mem") == 0) {
          int64_t* cur = nullptr;
          int64_t* end = nullptr;
          int next_arg = 1;

          if (strcmp(cmd, "stack") == 0) {
            cur = reinterpret_cast<int64_t*>(sim_->get_register(Simulator::sp));
          } else {  // Command "mem".
            int64_t value;
            if (!GetValue(arg1, &value)) {
              PrintF("%s unrecognized\n", arg1);
              continue;
            }
            cur = reinterpret_cast<int64_t*>(value);
            next_arg++;
          }

          int64_t words;
          if (argc == next_arg) {
            words = 10;
          } else {
            if (!GetValue(argv[next_arg], &words)) {
              words = 10;
            }
          }
          end = cur + words;

          while (cur < end) {
            PrintF("  0x%012" PRIxPTR " :  0x%016" PRIx64 "  %14" PRId64 " ",
                   reinterpret_cast<intptr_t>(cur), *cur, *cur);
            Object obj(*cur);
            Heap* current_heap = sim_->isolate_->heap();
            if (obj.IsSmi() ||
                IsValidHeapObject(current_heap, HeapObject::cast(obj))) {
              PrintF(" (");
              if (obj.IsSmi()) {
                PrintF("smi %d", Smi::ToInt(obj));
              } else {
                obj.ShortPrint();
              }
              PrintF(")");
            }
            PrintF("\n");
            cur++;
          }

        } else if ((strcmp(cmd, "disasm") == 0) || (strcmp(cmd, "dpc") == 0) ||
                   (strcmp(cmd, "di") == 0)) {
          disasm::NameConverter converter;
          disasm::Disassembler dasm(converter);
          // Use a reasonably large buffer.
          v8::internal::EmbeddedVector<char, 256> buffer;

          byte* cur = nullptr;
          byte* end = nullptr;

          if (argc == 1) {
            cur = reinterpret_cast<byte*>(sim_->get_pc());
            end = cur + (10 * kInstrSize);
          } else if (argc == 2) {
            int regnum = Registers::Number(arg1);
            if (regnum != kInvalidRegister || strncmp(arg1, "0x", 2) == 0) {
              // The argument is an address or a register name.
              int64_t value;
              if (GetValue(arg1, &value)) {
                cur = reinterpret_cast<byte*>(value);
                // Disassemble 10 instructions at <arg1>.
                end = cur + (10 * kInstrSize);
              }
            } else {
              // The argument is the number of instructions.
              int64_t value;
              if (GetValue(arg1, &value)) {
                cur = reinterpret_cast<byte*>(sim_->get_pc());
                // Disassemble <arg1> instructions.
                end = cur + (value * kInstrSize);
              }
            }
          } else {
            int64_t value1;
            int64_t value2;
            if (GetValue(arg1, &value1) && GetValue(arg2, &value2)) {
              cur = reinterpret_cast<byte*>(value1);
              end = cur + (value2 * kInstrSize);
            }
          }

          while (cur < end) {
            dasm.InstructionDecode(buffer, cur);
            PrintF("  0x%08" PRIxPTR "   %s\n", reinterpret_cast<intptr_t>(cur),
                   buffer.begin());
            cur += kInstrSize;
          }
        } else if (strcmp(cmd, "gdb") == 0) {
          PrintF("relinquishing control to gdb\n");
          v8::base::OS::DebugBreak();
          PrintF("regaining control from gdb\n");
        } else if (strcmp(cmd, "break") == 0) {
          if (argc == 2) {
            int64_t value;
            if (GetValue(arg1, &value)) {
              if (!SetBreakpoint(reinterpret_cast<Instruction*>(value))) {
                PrintF("setting breakpoint failed\n");
              }
            } else {
              PrintF("%s unrecognized\n", arg1);
            }
          } else {
            PrintF("break <address>\n");
          }
        } else if (strcmp(cmd, "del") == 0) {
          if (!DeleteBreakpoint(nullptr)) {
            PrintF("deleting breakpoint failed\n");
          }
        } else if (strcmp(cmd, "flags") == 0) {
          PrintF("No flags on RISC-V !\n");
        } else if (strcmp(cmd, "stop") == 0) {
          int64_t value;
          intptr_t stop_pc = sim_->get_pc() - 2 * kInstrSize;
          Instruction* stop_instr = reinterpret_cast<Instruction*>(stop_pc);
          Instruction* msg_address =
              reinterpret_cast<Instruction*>(stop_pc + kInstrSize);
          if ((argc == 2) && (strcmp(arg1, "unstop") == 0)) {
            // Remove the current stop.
            if (sim_->IsStopInstruction(stop_instr)) {
              stop_instr->SetInstructionBits(kNopInstr);
              msg_address->SetInstructionBits(kNopInstr);
            } else {
              PrintF("Not at debugger stop.\n");
            }
          } else if (argc == 3) {
            // Print information about all/the specified breakpoint(s).
            if (strcmp(arg1, "info") == 0) {
              if (strcmp(arg2, "all") == 0) {
                PrintF("Stop information:\n");
                for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                     i++) {
                  sim_->PrintStopInfo(i);
                }
              } else if (GetValue(arg2, &value)) {
                sim_->PrintStopInfo(value);
              } else {
                PrintF("Unrecognized argument.\n");
              }
            } else if (strcmp(arg1, "enable") == 0) {
              // Enable all/the specified breakpoint(s).
              if (strcmp(arg2, "all") == 0) {
                for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                     i++) {
                  sim_->EnableStop(i);
                }
              } else if (GetValue(arg2, &value)) {
                sim_->EnableStop(value);
              } else {
                PrintF("Unrecognized argument.\n");
              }
            } else if (strcmp(arg1, "disable") == 0) {
              // Disable all/the specified breakpoint(s).
              if (strcmp(arg2, "all") == 0) {
                for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                     i++) {
                  sim_->DisableStop(i);
                }
              } else if (GetValue(arg2, &value)) {
                sim_->DisableStop(value);
              } else {
                PrintF("Unrecognized argument.\n");
              }
            }
          } else {
            PrintF("Wrong usage. Use help command for more information.\n");
          }
        } else if ((strcmp(cmd, "stat") == 0) || (strcmp(cmd, "st") == 0)) {
          // Print registers and disassemble.
          PrintAllRegs();
          PrintF("\n");

          disasm::NameConverter converter;
          disasm::Disassembler dasm(converter);
          // Use a reasonably large buffer.
          v8::internal::EmbeddedVector<char, 256> buffer;

          byte* cur = nullptr;
          byte* end = nullptr;

          if (argc == 1) {
            cur = reinterpret_cast<byte*>(sim_->get_pc());
            end = cur + (10 * kInstrSize);
          } else if (argc == 2) {
            int64_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(value);
              // no length parameter passed, assume 10 instructions
              end = cur + (10 * kInstrSize);
            }
          } else {
            int64_t value1;
            int64_t value2;
            if (GetValue(arg1, &value1) && GetValue(arg2, &value2)) {
              cur = reinterpret_cast<byte*>(value1);
              end = cur + (value2 * kInstrSize);
            }
          }

          while (cur < end) {
            dasm.InstructionDecode(buffer, cur);
            PrintF("  0x%08" PRIxPTR "   %s\n", reinterpret_cast<intptr_t>(cur),
                   buffer.begin());
            cur += kInstrSize;
          }
        } else if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0)) {
          PrintF("cont\n");
          PrintF("  continue execution (alias 'c')\n");
          PrintF("stepi\n");
          PrintF("  step one instruction (alias 'si')\n");
          PrintF("print <register>\n");
          PrintF("  print register content (alias 'p')\n");
          PrintF("  use register name 'all' to print all registers\n");
          PrintF("printobject <register>\n");
          PrintF("  print an object from a register (alias 'po')\n");
          PrintF("stack [<words>]\n");
          PrintF("  dump stack content, default dump 10 words)\n");
          PrintF("mem <address> [<words>]\n");
          PrintF("  dump memory content, default dump 10 words)\n");
          PrintF("flags\n");
          PrintF("  print flags\n");
          PrintF("disasm [<instructions>]\n");
          PrintF("disasm [<address/register>]\n");
          PrintF("disasm [[<address/register>] <instructions>]\n");
          PrintF("  disassemble code, default is 10 instructions\n");
          PrintF("  from pc (alias 'di')\n");
          PrintF("gdb\n");
          PrintF("  enter gdb\n");
          PrintF("break <address>\n");
          PrintF("  set a break point on the address\n");
          PrintF("del\n");
          PrintF("  delete the breakpoint\n");
          PrintF("stop feature:\n");
          PrintF("  Description:\n");
          PrintF("    Stops are debug instructions inserted by\n");
          PrintF("    the Assembler::stop() function.\n");
          PrintF("    When hitting a stop, the Simulator will\n");
          PrintF("    stop and give control to the Debugger.\n");
          PrintF("    All stop codes are watched:\n");
          PrintF("    - They can be enabled / disabled: the Simulator\n");
          PrintF("       will / won't stop when hitting them.\n");
          PrintF("    - The Simulator keeps track of how many times they \n");
          PrintF("      are met. (See the info command.) Going over a\n");
          PrintF("      disabled stop still increases its counter. \n");
          PrintF("  Commands:\n");
          PrintF("    stop info all/<code> : print infos about number
  <code>\n"); PrintF("      or all stop(s).\n"); PrintF("    stop enable/disable
  all/<code> : enables / disables\n"); PrintF("      all or number <code>
  stop(s)\n"); PrintF("    stop unstop\n"); PrintF("      ignore the stop
  instruction at the current location\n"); PrintF("      from now on\n"); } else
  { PrintF("Unknown command: %s\n", cmd);
        }
      }
    }

    // Add all the breakpoints back to stop execution and enter the debugger
    // shell when hit.
    RedoBreakpoints();

  #undef COMMAND_SIZE
  #undef ARG_SIZE

  #undef STR
  #undef XSTR
  */
}

bool Simulator::ICacheMatch(void* one, void* two) {
  DCHECK_EQ(reinterpret_cast<intptr_t>(one) & CachePage::kPageMask, 0);
  DCHECK_EQ(reinterpret_cast<intptr_t>(two) & CachePage::kPageMask, 0);
  return one == two;
}

static uint32_t ICacheHash(void* key) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(key)) >> 2;
}

static bool AllOnOnePage(uintptr_t start, size_t size) {
  intptr_t start_page = (start & ~CachePage::kPageMask);
  intptr_t end_page = ((start + size) & ~CachePage::kPageMask);
  return start_page == end_page;
}

void Simulator::set_last_debugger_input(char* input) {
  DeleteArray(last_debugger_input_);
  last_debugger_input_ = input;
}

void Simulator::SetRedirectInstruction(Instruction* instruction) {
  instruction->SetInstructionBits(rtCallRedirInstr);
}

void Simulator::FlushICache(base::CustomMatcherHashMap* i_cache,
                            void* start_addr, size_t size) {
  int64_t start = reinterpret_cast<int64_t>(start_addr);
  int64_t intra_line = (start & CachePage::kLineMask);
  start -= intra_line;
  size += intra_line;
  size = ((size - 1) | CachePage::kLineMask) + 1;
  int offset = (start & CachePage::kPageMask);
  while (!AllOnOnePage(start, size - 1)) {
    int bytes_to_flush = CachePage::kPageSize - offset;
    FlushOnePage(i_cache, start, bytes_to_flush);
    start += bytes_to_flush;
    size -= bytes_to_flush;
    DCHECK_EQ((int64_t)0, start & CachePage::kPageMask);
    offset = 0;
  }
  if (size != 0) {
    FlushOnePage(i_cache, start, size);
  }
}

CachePage* Simulator::GetCachePage(base::CustomMatcherHashMap* i_cache,
                                   void* page) {
  base::HashMap::Entry* entry = i_cache->LookupOrInsert(page, ICacheHash(page));
  if (entry->value == nullptr) {
    CachePage* new_page = new CachePage();
    entry->value = new_page;
  }
  return reinterpret_cast<CachePage*>(entry->value);
}

// Flush from start up to and not including start + size.
void Simulator::FlushOnePage(base::CustomMatcherHashMap* i_cache,
                             intptr_t start, size_t size) {
  DCHECK_LE(size, CachePage::kPageSize);
  DCHECK(AllOnOnePage(start, size - 1));
  DCHECK_EQ(start & CachePage::kLineMask, 0);
  DCHECK_EQ(size & CachePage::kLineMask, 0);
  void* page = reinterpret_cast<void*>(start & (~CachePage::kPageMask));
  int offset = (start & CachePage::kPageMask);
  CachePage* cache_page = GetCachePage(i_cache, page);
  char* valid_bytemap = cache_page->ValidityByte(offset);
  memset(valid_bytemap, CachePage::LINE_INVALID, size >> CachePage::kLineShift);
}

void Simulator::CheckICache(base::CustomMatcherHashMap* i_cache,
                            Instruction* instr) {
  int64_t address = reinterpret_cast<int64_t>(instr);
  void* page = reinterpret_cast<void*>(address & (~CachePage::kPageMask));
  void* line = reinterpret_cast<void*>(address & (~CachePage::kLineMask));
  int offset = (address & CachePage::kPageMask);
  CachePage* cache_page = GetCachePage(i_cache, page);
  char* cache_valid_byte = cache_page->ValidityByte(offset);
  bool cache_hit = (*cache_valid_byte == CachePage::LINE_VALID);
  char* cached_line = cache_page->CachedData(offset & ~CachePage::kLineMask);
  if (cache_hit) {
    // Check that the data in memory matches the contents of the I-cache.
    CHECK_EQ(0, memcmp(reinterpret_cast<void*>(instr),
                       cache_page->CachedData(offset), kInstrSize));
  } else {
    // Cache miss.  Load memory into the cache.
    memcpy(cached_line, line, CachePage::kLineLength);
    *cache_valid_byte = CachePage::LINE_VALID;
  }
}

Simulator::Simulator(Isolate* isolate) : isolate_(isolate) {
  // Set up simulator support first. Some of this information is needed to
  // setup the architecture state.
  stack_size_ = FLAG_sim_stack_size * KB;
  stack_ = reinterpret_cast<char*>(malloc(stack_size_));
  pc_modified_ = false;
  icount_ = 0;
  break_count_ = 0;
  break_pc_ = nullptr;
  break_instr_ = 0;

  // Set up architecture state.
  // All registers are initialized to zero to start with.
  for (int i = 0; i < kNumSimuRegisters; i++) {
    registers_[i] = 0;
  }

  // FIXME (RISCV): remove MSA ASE part?
  for (int i = 0; i < kNumFPURegisters; i++) {
    FPUregisters_[2 * i] = 0;
    FPUregisters_[2 * i + 1] = 0;  // upper part for MSA ASE
  }

  FCSR_ = 0;

  // The sp is initialized to point to the bottom (high address) of the
  // allocated stack area. To be safe in potential stack underflows we leave
  // some buffer below.
  registers_[sp] = reinterpret_cast<int64_t>(stack_) + stack_size_ - 64;
  // The ra and pc are initialized to a known bad value that will cause an
  // access violation if the simulator ever tries to execute it.
  registers_[pc] = bad_ra;
  registers_[ra] = bad_ra;

  last_debugger_input_ = nullptr;
}

Simulator::~Simulator() {
  GlobalMonitor::Get()->RemoveLinkedAddress(&global_monitor_thread_);
  free(stack_);
}

// Get the active Simulator for the current thread.
Simulator* Simulator::current(Isolate* isolate) {
  v8::internal::Isolate::PerIsolateThreadData* isolate_data =
      isolate->FindOrAllocatePerThreadDataForThisThread();
  DCHECK_NOT_NULL(isolate_data);

  Simulator* sim = isolate_data->simulator();
  if (sim == nullptr) {
    // TODO(146): delete the simulator object when a thread/isolate goes away.
    sim = new Simulator(isolate);
    isolate_data->set_simulator(sim);
  }
  return sim;
}

// Sets the register in the architecture state. It will also deal with updating
// Simulator internal state for special registers such as PC.
void Simulator::set_register(int reg, int64_t value) {
  DCHECK((reg >= 0) && (reg < kNumSimuRegisters));
  if (reg == pc) {
    pc_modified_ = true;
  }

  // Zero register always holds 0.
  registers_[reg] = (reg == 0) ? 0 : value;
}

void Simulator::set_dw_register(int reg, const int* dbl) {
  DCHECK((reg >= 0) && (reg < kNumSimuRegisters));
  registers_[reg] = dbl[1];
  registers_[reg] = registers_[reg] << 32;
  registers_[reg] += dbl[0];
}

void Simulator::set_fpu_register(int fpureg, int64_t value) {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  FPUregisters_[fpureg * 2] = value;
}

void Simulator::set_fpu_register_word(int fpureg, int32_t value) {
  // Set ONLY lower 32-bits, leaving upper bits untouched.
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  int32_t* pword;
  if (kArchEndian == kLittle) {
    pword = reinterpret_cast<int32_t*>(&FPUregisters_[fpureg * 2]);
  } else {
    pword = reinterpret_cast<int32_t*>(&FPUregisters_[fpureg * 2]) + 1;
  }
  *pword = value;
}

void Simulator::set_fpu_register_hi_word(int fpureg, int32_t value) {
  // Set ONLY upper 32-bits, leaving lower bits untouched.
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  int32_t* phiword;
  if (kArchEndian == kLittle) {
    phiword = (reinterpret_cast<int32_t*>(&FPUregisters_[fpureg * 2])) + 1;
  } else {
    phiword = reinterpret_cast<int32_t*>(&FPUregisters_[fpureg * 2]);
  }
  *phiword = value;
}

void Simulator::set_fpu_register_float(int fpureg, float value) {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  *bit_cast<float*>(&FPUregisters_[fpureg * 2]) = value;
}

void Simulator::set_fpu_register_double(int fpureg, double value) {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  *bit_cast<double*>(&FPUregisters_[fpureg * 2]) = value;
}

// Get the register from the architecture state. This function does handle
// the special case of accessing the PC register.
int64_t Simulator::get_register(int reg) const {
  DCHECK((reg >= 0) && (reg < kNumSimuRegisters));
  if (reg == 0)
    return 0;
  else
    return registers_[reg] + ((reg == pc) ? Instruction::kPCReadOffset : 0);
}

double Simulator::get_double_from_register_pair(int reg) {
  // TODO(plind): bad ABI stuff, refactor or remove.
  DCHECK((reg >= 0) && (reg < kNumSimuRegisters) && ((reg % 2) == 0));

  double dm_val = 0.0;
  // Read the bits from the unsigned integer register_[] array
  // into the double precision floating point value and return it.
  char buffer[sizeof(registers_[0])];
  memcpy(buffer, &registers_[reg], sizeof(registers_[0]));
  memcpy(&dm_val, buffer, sizeof(registers_[0]));
  return (dm_val);
}

int64_t Simulator::get_fpu_register(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return FPUregisters_[fpureg * 2];
}

int32_t Simulator::get_fpu_register_word(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return static_cast<int32_t>(FPUregisters_[fpureg * 2] & 0xFFFFFFFF);
}

int32_t Simulator::get_fpu_register_signed_word(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return static_cast<int32_t>(FPUregisters_[fpureg * 2] & 0xFFFFFFFF);
}

int32_t Simulator::get_fpu_register_hi_word(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return static_cast<int32_t>((FPUregisters_[fpureg * 2] >> 32) & 0xFFFFFFFF);
}

float Simulator::get_fpu_register_float(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return *bit_cast<float*>(const_cast<int64_t*>(&FPUregisters_[fpureg * 2]));
}

double Simulator::get_fpu_register_double(int fpureg) const {
  DCHECK((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return *bit_cast<double*>(&FPUregisters_[fpureg * 2]);
}

// Runtime FP routines take up to two double arguments and zero
// or one integer arguments. All are constructed here,
// from a0-a3 or fa0 and fa1 (n64), or fa2 (O32).
void Simulator::GetFpArgs(double* x, double* y, int32_t* z) {
  if (!IsMipsSoftFloatABI) {
    const int fparg2 = 13;
    *x = get_fpu_register_double(12);
    *y = get_fpu_register_double(fparg2);
    *z = static_cast<int32_t>(get_register(a2));
  } else {
    // FIXME (RISCV): a0,... are 64-bit, why move them to 32-bit buffer?

    // TODO(plind): bad ABI stuff, refactor or remove.
    // We use a char buffer to get around the strict-aliasing rules which
    // otherwise allow the compiler to optimize away the copy.
    char buffer[sizeof(*x)];
    int32_t* reg_buffer = reinterpret_cast<int32_t*>(buffer);

    // Registers a0 and a1 -> x.
    reg_buffer[0] = get_register(a0);
    reg_buffer[1] = get_register(a1);
    memcpy(x, buffer, sizeof(buffer));
    // Registers a2 and a3 -> y.
    reg_buffer[0] = get_register(a2);
    reg_buffer[1] = get_register(a3);
    memcpy(y, buffer, sizeof(buffer));
    // Register 2 -> z.
    reg_buffer[0] = get_register(a2);
    memcpy(z, buffer, sizeof(*z));
  }
}

// The return value is either in a0/a1 or ft0.
void Simulator::SetFpResult(const double& result) {
  if (!IsMipsSoftFloatABI) {
    set_fpu_register_double(0, result);
  } else {
    char buffer[2 * sizeof(registers_[0])];
    int64_t* reg_buffer = reinterpret_cast<int64_t*>(buffer);
    memcpy(buffer, &result, sizeof(buffer));
    // Copy result to a0 and a1.
    set_register(a0, reg_buffer[0]);
    set_register(a1, reg_buffer[1]);
  }
}

// helper functions to read/write/set/clear CRC values/bits
uint32_t Simulator::read_csr_value(uint32_t csr) {
  switch (csr) {
    case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
      return (FCSR_ & kFcsrFlagsMask);
    case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
      return (FCSR_ & kFcsrFrmMask) >> kFcsrFrmShift;
    case csr_fcsr:  // Floating-Point Control and Status Register (RW)
      return (FCSR_ & kFcsrMask);
    default:
      UNIMPLEMENTED();
  }
}

uint32_t Simulator::get_dynamic_rounding_mode() {
  return read_csr_value(csr_frm);
}

void Simulator::write_csr_value(uint32_t csr, uint64_t val) {
  uint32_t value = (uint32_t)val;
  switch (csr) {
    case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
      DCHECK(value <= ((1 << kFcsrFlagsBits) - 1));
      FCSR_ = (FCSR_ & (~kFcsrFlagsMask)) | value;
      break;
    case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
      DCHECK(value <= ((1 << kFcsrFrmBits) - 1));
      FCSR_ = (FCSR_ & (~kFcsrFrmMask)) | (value << kFcsrFrmShift);
      break;
    case csr_fcsr:  // Floating-Point Control and Status Register (RW)
      DCHECK(value <= ((1 << kFcsrBits) - 1));
      FCSR_ = (FCSR_ & (~kFcsrMask)) | value;
      break;
    default:
      UNIMPLEMENTED();
  }
}

void Simulator::set_csr_bits(uint32_t csr, uint64_t val) {
  uint32_t value = (uint32_t)val;
  switch (csr) {
    case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
      DCHECK(value <= ((1 << kFcsrFlagsBits) - 1));
      FCSR_ = FCSR_ | value;
      break;
    case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
      DCHECK(value <= ((1 << kFcsrFrmBits) - 1));
      FCSR_ = FCSR_ | (value << kFcsrFrmShift);
      break;
    case csr_fcsr:  // Floating-Point Control and Status Register (RW)
      DCHECK(value <= ((1 << kFcsrBits) - 1));
      FCSR_ = FCSR_ | value;
      break;
    default:
      UNIMPLEMENTED();
  }
}

void Simulator::clear_csr_bits(uint32_t csr, uint64_t val) {
  uint32_t value = (uint32_t)val;
  switch (csr) {
    case csr_fflags:  // Floating-Point Accrued Exceptions (RW)
      DCHECK(value <= ((1 << kFcsrFlagsBits) - 1));
      FCSR_ = FCSR_ & (~value);
      break;
    case csr_frm:  // Floating-Point Dynamic Rounding Mode (RW)
      DCHECK(value <= ((1 << kFcsrFrmBits) - 1));
      FCSR_ = FCSR_ & (~(value << kFcsrFrmShift));
      break;
    case csr_fcsr:  // Floating-Point Control and Status Register (RW)
      DCHECK(value <= ((1 << kFcsrBits) - 1));
      FCSR_ = FCSR_ & (~value);
      break;
    default:
      UNIMPLEMENTED();
  }
}

bool Simulator::test_fflags_bits(uint32_t mask) {
  return (FCSR_ & kFcsrFlagsMask & mask) != 0;
}

template <typename T>
T Simulator::FMaxMinHelper(T a, T b, MaxMinKind kind) {
  // set invalid bit for signaling nan
  if ((a == std::numeric_limits<T>::signaling_NaN()) ||
      (b == std::numeric_limits<T>::signaling_NaN())) {
    // FIXME: NV -> kInvalidOperation
    set_csr_bits(csr_fflags, kInvalidOperation);
  }

  T result = 0;
  if (std::isnan(a) && std::isnan(b)) {
    result = a;
  } else if (std::isnan(a)) {
    result = b;
  } else if (std::isnan(b)) {
    result = a;
  } else if (b == a) {  // Handle -0.0 == 0.0 case.
    if (kind == MaxMinKind::kMax) {
      result = std::signbit(b) ? a : b;
    } else {
      result = std::signbit(b) ? b : a;
    }
  } else {
    result = (kind == MaxMinKind::kMax) ? fmax(a, b) : fmin(a, b);
  }

  return result;
}

// Raw access to the PC register.
void Simulator::set_pc(int64_t value) {
  pc_modified_ = true;
  registers_[pc] = value;
  DCHECK(has_bad_pc() || ((value % kInstrSize) == 0));
}

bool Simulator::has_bad_pc() const {
  return ((registers_[pc] == bad_ra) || (registers_[pc] == end_sim_pc));
}

// Raw access to the PC register without the special adjustment when reading.
int64_t Simulator::get_pc() const { return registers_[pc]; }

// The MIPS cannot do unaligned reads and writes.  On some MIPS platforms an
// interrupt is caused.  On others it does a funky rotation thing.  For now we
// simply disallow unaligned reads, but at some point we may want to move to
// emulating the rotate behaviour.  Note that simulator runs have the runtime
// system running directly on the host system and only generated code is
// executed in the simulator.  Since the host is typically IA32 we will not
// get the correct MIPS-like behaviour on unaligned accesses.

// TODO(plind): refactor this messy debug code when we do unaligned access.
void Simulator::DieOrDebug() {
  if ((1)) {  // Flag for this was removed.
    RiscvDebugger dbg(this);
    dbg.Debug();
  } else {
    base::OS::Abort();
  }
}

void Simulator::TraceRegWr(int64_t value, TraceType t) {
  if (::v8::internal::FLAG_trace_sim) {
    union {
      int64_t fmt_int64;
      int32_t fmt_int32[2];
      float fmt_float[2];
      double fmt_double;
    } v;
    v.fmt_int64 = value;

    switch (t) {
      case WORD:
        SNPrintF(trace_buf_,
                 "%016" PRIx64 "    (%" PRId64 ")    int32:%" PRId32
                 " uint32:%" PRIu32,
                 v.fmt_int64, icount_, v.fmt_int32[0], v.fmt_int32[0]);
        break;
      case DWORD:
        SNPrintF(trace_buf_,
                 "%016" PRIx64 "    (%" PRId64 ")    int64:%" PRId64
                 " uint64:%" PRIu64,
                 value, icount_, value, value);
        break;
      case FLOAT:
        SNPrintF(trace_buf_, "%016" PRIx64 "    (%" PRId64 ")    flt:%e",
                 v.fmt_int64, icount_, v.fmt_float[0]);
        break;
      case DOUBLE:
        SNPrintF(trace_buf_, "%016" PRIx64 "    (%" PRId64 ")    dbl:%e",
                 v.fmt_int64, icount_, v.fmt_double);
        break;
      default:
        UNREACHABLE();
    }
  }
}

// TODO(plind): consider making icount_ printing a flag option.
template <typename T>
void Simulator::TraceMemRd(int64_t addr, T value, int64_t reg_value) {
  if (::v8::internal::FLAG_trace_sim) {
    if (std::is_integral<T>::value) {
      switch (sizeof(T)) {
        case 1:
          SNPrintF(trace_buf_,
                   "%016" PRIx64 "    (%" PRId64 ")    int8:%" PRId8
                   " uint8:%" PRIu8 " <-- [addr: %" PRIx64 "]",
                   reg_value, icount_, static_cast<int8_t>(value),
                   static_cast<uint8_t>(value), addr);
          break;
        case 2:
          SNPrintF(trace_buf_,
                   "%016" PRIx64 "    (%" PRId64 ")    int16:%" PRId16
                   " uint16:%" PRIu16 " <-- [addr: %" PRIx64 "]",
                   reg_value, icount_, static_cast<int16_t>(value),
                   static_cast<uint16_t>(value), addr);
          break;
        case 4:
          SNPrintF(trace_buf_,
                   "%016" PRIx64 "    (%" PRId64 ")    int32:%" PRId32
                   " uint32:%" PRIu32 " <-- [addr: %" PRIx64 "]",
                   reg_value, icount_, static_cast<int32_t>(value),
                   static_cast<uint32_t>(value), addr);
          break;
        case 8:
          SNPrintF(trace_buf_,
                   "%016" PRIx64 "    (%" PRId64 ")    int64:%" PRId64
                   " uint64:%" PRIu64 " <-- [addr: %" PRIx64 "]",
                   reg_value, icount_, static_cast<int64_t>(value),
                   static_cast<uint64_t>(value), addr);
          break;
        default:
          UNREACHABLE();
      }
    } else if (std::is_same<float, T>::value) {
      SNPrintF(trace_buf_,
               "%016" PRIx64 "    (%" PRId64 ")    flt:%e <-- [addr: %" PRIx64
               "]",
               reg_value, icount_, static_cast<float>(value), addr);
    } else if (std::is_same<double, T>::value) {
      SNPrintF(trace_buf_,
               "%016" PRIx64 "    (%" PRId64 ")    dbl:%e <-- [addr: %" PRIx64
               "]",
               reg_value, icount_, static_cast<double>(value), addr);
    } else {
      UNREACHABLE();
    }
  }
}

template <typename T>
void Simulator::TraceMemWr(int64_t addr, T value) {
  if (::v8::internal::FLAG_trace_sim) {
    switch (sizeof(T)) {
      case 1:
        SNPrintF(trace_buf_,
                 "                    (%" PRIu64 ")    int8:%" PRId8
                 " uint8:%" PRIu8 " --> [addr: %" PRIx64 "]",
                 icount_, static_cast<int8_t>(value),
                 static_cast<uint8_t>(value), addr);
        break;
      case 2:
        SNPrintF(trace_buf_,
                 "                    (%" PRIu64 ")    int16:%" PRId16
                 " uint16:%" PRIu16 " --> [addr: %" PRIx64 "]",
                 icount_, static_cast<int16_t>(value),
                 static_cast<uint16_t>(value), addr);
        break;
      case 4:
        if (std::is_integral<T>::value) {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64 ")    int32:%" PRId32
                   " uint32:%" PRIu32 " --> [addr: %" PRIx64 "]",
                   icount_, static_cast<int32_t>(value),
                   static_cast<uint32_t>(value), addr);
        } else {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64
                   ")    flt:%e --> [addr: %" PRIx64 "]",
                   icount_, static_cast<float>(value), addr);
        }
        break;
      case 8:
        if (std::is_integral<T>::value) {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64 ")    int64:%" PRId64
                   " uint64:%" PRIu64 " --> [addr: %" PRIx64 "]",
                   icount_, static_cast<int64_t>(value),
                   static_cast<uint64_t>(value), addr);
        } else {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64
                   ")    dbl:%e --> [addr: %" PRIx64 "]",
                   icount_, static_cast<double>(value), addr);
        }
        break;
      default:
        UNREACHABLE();
    }
  }
}

// RISCV Memory Read/Write functions
// Compared with mips64, RISCV can support unaligned read/write. EEI decides.
// FIXME: RISCV porting: Add boundary check and TraceMem* support
// FIXME: our target board traps on unaligned loads, so need to add detection
// of unaligned load/store

template <typename T>
T Simulator::RV_ReadMem(int64_t addr, Instruction* instr) {
  if (addr >= 0 && addr < 0x400) {
    // This has to be a nullptr-dereference, drop into debugger.
    PrintF("Memory read from bad address: 0x%08" PRIx64 " , pc=0x%08" PRIxPTR
           " \n",
           addr, reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }

  // check for natural alignment
  if ((addr & (sizeof(T) - 1)) != 0) {
    PrintF("Unaligned read at 0x%08" PRIx64 " , pc=0x%08" V8PRIxPTR "\n", addr,
           reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }

  T* ptr = reinterpret_cast<T*>(addr);
  T value = *ptr;
  return value;
}

template <typename T>
void Simulator::RV_WriteMem(int64_t addr, T value, Instruction* instr) {
  if (addr >= 0 && addr < 0x400) {
    // This has to be a nullptr-dereference, drop into debugger.
    PrintF("Memory write to bad address: 0x%08" PRIx64 " , pc=0x%08" PRIxPTR
           " \n",
           addr, reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }

  // check for natural alignment
  if ((addr & (sizeof(T) - 1)) != 0) {
    PrintF("Unaligned write at 0x%08" PRIx64 " , pc=0x%08" V8PRIxPTR "\n", addr,
           reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }

  T* ptr = reinterpret_cast<T*>(addr);
  TraceMemWr(addr, value);
  *ptr = value;
}

// Returns the limit of the stack area to enable checking for stack overflows.
uintptr_t Simulator::StackLimit(uintptr_t c_limit) const {
  // The simulator uses a separate JS stack. If we have exhausted the C stack,
  // we also drop down the JS limit to reflect the exhaustion on the JS stack.
  if (GetCurrentStackPosition() < c_limit) {
    return reinterpret_cast<uintptr_t>(get_sp());
  }

  // Otherwise the limit is the JS stack. Leave a safety margin of 1024 bytes
  // to prevent overrunning the stack when pushing values.
  return reinterpret_cast<uintptr_t>(stack_) + 1024;
}

// Unsupported instructions use Format to print an error and stop execution.
void Simulator::Format(Instruction* instr, const char* format) {
  PrintF("Simulator found unsupported instruction:\n 0x%08" PRIxPTR " : %s\n",
         reinterpret_cast<intptr_t>(instr), format);
  UNIMPLEMENTED_RISCV();
}

// Calls into the V8 runtime are based on this very simple interface.
// Note: To be able to return two values from some calls the code in runtime.cc
// uses the ObjectPair which is essentially two 32-bit values stuffed into a
// 64-bit value. With the code below we assume that all runtime calls return
// 64 bits of result. If they don't, the a1 result register contains a bogus
// value, which is fine because it is caller-saved.

using SimulatorRuntimeCall = ObjectPair (*)(int64_t arg0, int64_t arg1,
                                            int64_t arg2, int64_t arg3,
                                            int64_t arg4, int64_t arg5,
                                            int64_t arg6, int64_t arg7,
                                            int64_t arg8, int64_t arg9);

// These prototypes handle the four types of FP calls.
using SimulatorRuntimeCompareCall = int64_t (*)(double darg0, double darg1);
using SimulatorRuntimeFPFPCall = double (*)(double darg0, double darg1);
using SimulatorRuntimeFPCall = double (*)(double darg0);
using SimulatorRuntimeFPIntCall = double (*)(double darg0, int32_t arg0);

// This signature supports direct call in to API function native callback
// (refer to InvocationCallback in v8.h).
using SimulatorRuntimeDirectApiCall = void (*)(int64_t arg0);
using SimulatorRuntimeProfilingApiCall = void (*)(int64_t arg0, void* arg1);

// This signature supports direct call to accessor getter callback.
using SimulatorRuntimeDirectGetterCall = void (*)(int64_t arg0, int64_t arg1);
using SimulatorRuntimeProfilingGetterCall = void (*)(int64_t arg0, int64_t arg1,
                                                     void* arg2);

// Software interrupt instructions are used by the simulator to call into the
// C-based V8 runtime. They are also used for debugging with simulator.
void Simulator::SoftwareInterrupt() {
  // There are two instructions that could get us here,
  // the ebreak instruction, or the ecall instruction. Both are
  // "SPECIAL" class opcode, and are distinuished by the immediate field.
  int32_t func = instr_.Imm12Value();
  // We first check if we met a call_rt_redirected.
  if (instr_.InstructionBits() == rtCallRedirInstr) {
    Redirection* redirection = Redirection::FromInstruction(instr_.instr());

    int64_t* stack_pointer = reinterpret_cast<int64_t*>(get_register(sp));

    int64_t arg0 = get_register(a0);
    int64_t arg1 = get_register(a1);
    int64_t arg2 = get_register(a2);
    int64_t arg3 = get_register(a3);
    int64_t arg4 = get_register(a4);
    int64_t arg5 = get_register(a5);
    int64_t arg6 = get_register(a6);
    int64_t arg7 = get_register(a7);
    int64_t arg8 = stack_pointer[0];
    int64_t arg9 = stack_pointer[1];
    STATIC_ASSERT(kMaxCParameters == 10);

    bool fp_call =
        (redirection->type() == ExternalReference::BUILTIN_FP_FP_CALL) ||
        (redirection->type() == ExternalReference::BUILTIN_COMPARE_CALL) ||
        (redirection->type() == ExternalReference::BUILTIN_FP_CALL) ||
        (redirection->type() == ExternalReference::BUILTIN_FP_INT_CALL);

    // This is dodgy but it works because the C entry stubs are never moved.
    // See comment in codegen-arm.cc and bug 1242173.
    int64_t saved_ra = get_register(ra);

    intptr_t external =
        reinterpret_cast<intptr_t>(redirection->external_function());

    // Based on CpuFeatures::IsSupported(FPU), Mips will use either hardware
    // FPU, or gcc soft-float routines. Hardware FPU is simulated in this
    // simulator. Soft-float has additional abstraction of ExternalReference,
    // to support serialization.
    if (fp_call) {
      double dval0, dval1;  // one or two double parameters
      int32_t ival;         // zero or one integer parameters
      int64_t iresult = 0;  // integer return value
      double dresult = 0;   // double return value
      GetFpArgs(&dval0, &dval1, &ival);
      SimulatorRuntimeCall generic_target =
          reinterpret_cast<SimulatorRuntimeCall>(external);
      if (::v8::internal::FLAG_trace_sim) {
        switch (redirection->type()) {
          case ExternalReference::BUILTIN_FP_FP_CALL:
          case ExternalReference::BUILTIN_COMPARE_CALL:
            PrintF("Call to host function at %p with args %f, %f",
                   reinterpret_cast<void*>(FUNCTION_ADDR(generic_target)),
                   dval0, dval1);
            break;
          case ExternalReference::BUILTIN_FP_CALL:
            PrintF("Call to host function at %p with arg %f",
                   reinterpret_cast<void*>(FUNCTION_ADDR(generic_target)),
                   dval0);
            break;
          case ExternalReference::BUILTIN_FP_INT_CALL:
            PrintF("Call to host function at %p with args %f, %d",
                   reinterpret_cast<void*>(FUNCTION_ADDR(generic_target)),
                   dval0, ival);
            break;
          default:
            UNREACHABLE();
            break;
        }
      }
      switch (redirection->type()) {
        case ExternalReference::BUILTIN_COMPARE_CALL: {
          SimulatorRuntimeCompareCall target =
              reinterpret_cast<SimulatorRuntimeCompareCall>(external);
          iresult = target(dval0, dval1);
          set_register(a0, static_cast<int64_t>(iresult));
          //  set_register(a1, static_cast<int64_t>(iresult >> 32));
          break;
        }
        case ExternalReference::BUILTIN_FP_FP_CALL: {
          SimulatorRuntimeFPFPCall target =
              reinterpret_cast<SimulatorRuntimeFPFPCall>(external);
          dresult = target(dval0, dval1);
          SetFpResult(dresult);
          break;
        }
        case ExternalReference::BUILTIN_FP_CALL: {
          SimulatorRuntimeFPCall target =
              reinterpret_cast<SimulatorRuntimeFPCall>(external);
          dresult = target(dval0);
          SetFpResult(dresult);
          break;
        }
        case ExternalReference::BUILTIN_FP_INT_CALL: {
          SimulatorRuntimeFPIntCall target =
              reinterpret_cast<SimulatorRuntimeFPIntCall>(external);
          dresult = target(dval0, ival);
          SetFpResult(dresult);
          break;
        }
        default:
          UNREACHABLE();
          break;
      }
      if (::v8::internal::FLAG_trace_sim) {
        switch (redirection->type()) {
          case ExternalReference::BUILTIN_COMPARE_CALL:
            PrintF("Returned %08x\n", static_cast<int32_t>(iresult));
            break;
          case ExternalReference::BUILTIN_FP_FP_CALL:
          case ExternalReference::BUILTIN_FP_CALL:
          case ExternalReference::BUILTIN_FP_INT_CALL:
            PrintF("Returned %f\n", dresult);
            break;
          default:
            UNREACHABLE();
            break;
        }
      }
    } else if (redirection->type() == ExternalReference::DIRECT_API_CALL) {
      if (::v8::internal::FLAG_trace_sim) {
        PrintF("Call to host function at %p args %08" PRIx64 " \n",
               reinterpret_cast<void*>(external), arg0);
      }
      SimulatorRuntimeDirectApiCall target =
          reinterpret_cast<SimulatorRuntimeDirectApiCall>(external);
      target(arg0);
    } else if (redirection->type() == ExternalReference::PROFILING_API_CALL) {
      if (::v8::internal::FLAG_trace_sim) {
        PrintF("Call to host function at %p args %08" PRIx64 "  %08" PRIx64
               " \n",
               reinterpret_cast<void*>(external), arg0, arg1);
      }
      SimulatorRuntimeProfilingApiCall target =
          reinterpret_cast<SimulatorRuntimeProfilingApiCall>(external);
      target(arg0, Redirection::ReverseRedirection(arg1));
    } else if (redirection->type() == ExternalReference::DIRECT_GETTER_CALL) {
      if (::v8::internal::FLAG_trace_sim) {
        PrintF("Call to host function at %p args %08" PRIx64 "  %08" PRIx64
               " \n",
               reinterpret_cast<void*>(external), arg0, arg1);
      }
      SimulatorRuntimeDirectGetterCall target =
          reinterpret_cast<SimulatorRuntimeDirectGetterCall>(external);
      target(arg0, arg1);
    } else if (redirection->type() ==
               ExternalReference::PROFILING_GETTER_CALL) {
      if (::v8::internal::FLAG_trace_sim) {
        PrintF("Call to host function at %p args %08" PRIx64 "  %08" PRIx64
               "  %08" PRIx64 " \n",
               reinterpret_cast<void*>(external), arg0, arg1, arg2);
      }
      SimulatorRuntimeProfilingGetterCall target =
          reinterpret_cast<SimulatorRuntimeProfilingGetterCall>(external);
      target(arg0, arg1, Redirection::ReverseRedirection(arg2));
    } else {
      DCHECK(redirection->type() == ExternalReference::BUILTIN_CALL ||
             redirection->type() == ExternalReference::BUILTIN_CALL_PAIR);
      SimulatorRuntimeCall target =
          reinterpret_cast<SimulatorRuntimeCall>(external);
      if (::v8::internal::FLAG_trace_sim) {
        PrintF(
            "Call to host function at %p "
            "args %08" PRIx64 " , %08" PRIx64 " , %08" PRIx64 " , %08" PRIx64
            " , %08" PRIx64 " , %08" PRIx64 " , %08" PRIx64 " , %08" PRIx64
            " , %08" PRIx64 " , %08" PRIx64 " \n",
            reinterpret_cast<void*>(FUNCTION_ADDR(target)), arg0, arg1, arg2,
            arg3, arg4, arg5, arg6, arg7, arg8, arg9);
      }
      ObjectPair result =
          target(arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9);
      set_register(a0, (int64_t)(result.x));
      set_register(a1, (int64_t)(result.y));
    }
    if (::v8::internal::FLAG_trace_sim) {
      PrintF("Returned %08" PRIx64 "  : %08" PRIx64 " \n", get_register(a1),
             get_register(a0));
    }
    set_register(ra, saved_ra);
    set_pc(get_register(ra));

  }
  // FIXME (RISCV): Need to handle ebreak instructions used by the debugger
  else if (func == 1) {
    UNIMPLEMENTED();
    /*
      if (IsWatchpoint(code)) {
        PrintWatchpoint(code);
      } else {
        IncreaseStopCounter(code);
        HandleStop(code, instr_.instr());
      }
    } else {
      // All remaining break_ codes, and all traps are handled here.
      RiscvDebugger dbg(this);
      dbg.Debug();
    */
  }
}

// Stop helper functions.
bool Simulator::IsWatchpoint(uint64_t code) {
  return (code <= kMaxWatchpointCode);
}

void Simulator::PrintWatchpoint(uint64_t code) {
  RiscvDebugger dbg(this);
  ++break_count_;
  PrintF("\n---- break %" PRId64 "  marker: %3d  (instr count: %8" PRId64
         " ) ----------"
         "----------------------------------",
         code, break_count_, icount_);
  dbg.PrintAllRegs();  // Print registers and continue running.
}

void Simulator::HandleStop(uint64_t code, Instruction* instr) {
  // Stop if it is enabled, otherwise go on jumping over the stop
  // and the message address.
  if (IsEnabledStop(code)) {
    RiscvDebugger dbg(this);
    dbg.Stop(instr);
  }
}

// FIXME (RISCV): to be ported
bool Simulator::IsStopInstruction(Instruction* instr) {
  UNIMPLEMENTED();
  /*
  int32_t func = instr->FunctionFieldRaw();
  uint32_t code = static_cast<uint32_t>(instr->Bits(25, 6));
  return (func == BREAK) && code > kMaxWatchpointCode && code <= kMaxStopCode;
  */
}

bool Simulator::IsEnabledStop(uint64_t code) {
  DCHECK_LE(code, kMaxStopCode);
  DCHECK_GT(code, kMaxWatchpointCode);
  return !(watched_stops_[code].count & kStopDisabledBit);
}

void Simulator::EnableStop(uint64_t code) {
  if (!IsEnabledStop(code)) {
    watched_stops_[code].count &= ~kStopDisabledBit;
  }
}

void Simulator::DisableStop(uint64_t code) {
  if (IsEnabledStop(code)) {
    watched_stops_[code].count |= kStopDisabledBit;
  }
}

void Simulator::IncreaseStopCounter(uint64_t code) {
  DCHECK_LE(code, kMaxStopCode);
  if ((watched_stops_[code].count & ~(1 << 31)) == 0x7FFFFFFF) {
    PrintF("Stop counter for code %" PRId64
           "  has overflowed.\n"
           "Enabling this code and reseting the counter to 0.\n",
           code);
    watched_stops_[code].count = 0;
    EnableStop(code);
  } else {
    watched_stops_[code].count++;
  }
}

// Print a stop status.
void Simulator::PrintStopInfo(uint64_t code) {
  if (code <= kMaxWatchpointCode) {
    PrintF("That is a watchpoint, not a stop.\n");
    return;
  } else if (code > kMaxStopCode) {
    PrintF("Code too large, only %u stops can be used\n", kMaxStopCode + 1);
    return;
  }
  const char* state = IsEnabledStop(code) ? "Enabled" : "Disabled";
  int32_t count = watched_stops_[code].count & ~kStopDisabledBit;
  // Don't print the state of unused breakpoints.
  if (count != 0) {
    if (watched_stops_[code].desc) {
      PrintF("stop %" PRId64 "  - 0x%" PRIx64 " : \t%s, \tcounter = %i, \t%s\n",
             code, code, state, count, watched_stops_[code].desc);
    } else {
      PrintF("stop %" PRId64 "  - 0x%" PRIx64 " : \t%s, \tcounter = %i\n", code,
             code, state, count);
    }
  }
}

void Simulator::SignalException(Exception e) {
  FATAL("Error: Exception %i raised.", static_cast<int>(e));
}

// RISCV Instruction Decode Routine
void Simulator::DecodeRVRType() {
  switch (instr_.InstructionBits() & kRTypeMask) {
    case RO_ADD: {
      set_rd(sext_xlen(rs1() + rs2()));
      break;
    }
    case RO_SUB: {
      set_rd(sext_xlen(rs1() - rs2()));
      break;
    }
    case RO_SLL: {
      set_rd(sext_xlen(rs1() << (rs2() & (xlen - 1))));
      break;
    }
    case RO_SLT: {
      set_rd(sreg_t(rs1()) < sreg_t(rs2()));
      break;
    }
    case RO_SLTU: {
      set_rd(reg_t(rs1()) < reg_t(rs2()));
      break;
    }
    case RO_XOR: {
      set_rd(rs1() ^ rs2());
      break;
    }
    case RO_SRL: {
      set_rd(sext_xlen(zext_xlen(rs1()) >> (rs2() & (xlen - 1))));
      break;
    }
    case RO_SRA: {
      set_rd(sext_xlen(sext_xlen(rs1()) >> (rs2() & (xlen - 1))));
      break;
    }
    case RO_OR: {
      set_rd(rs1() | rs2());
      break;
    }
    case RO_AND: {
      set_rd(rs1() & rs2());
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_ADDW: {
      set_rd(sext32(rs1() + rs2()));
      break;
    }
    case RO_SUBW: {
      set_rd(sext32(rs1() - rs2()));
      break;
    }
    case RO_SLLW: {
      set_rd(sext32(rs1() << (rs2() & 0x1F)));
      break;
    }
    case RO_SRLW: {
      set_rd(sext32(uint32_t(rs1()) >> (rs2() & 0x1F)));
      break;
    }
    case RO_SRAW: {
      set_rd(sext32(int32_t(rs1()) >> (rs2() & 0x1F)));
      break;
    }
#endif /* V8_TARGET_ARCH_64_BIT */
      // TODO: Add RISCV M extension macro
    case RO_MUL: {
      set_rd(sext_xlen(rs1() * rs2()));
      break;
    }
    case RO_MULH: {
#ifdef V8_TARGET_ARCH_64_BIT
      set_rd(mulh(rs1(), rs2()));
#else
      set_rd(sext32((sext32(rs1()) * sext32(rs2())) >> 32));
#endif /*V8_TARGET_ARCH_64_BIT*/
      break;
    }
    case RO_MULHSU: {
#ifdef V8_TARGET_ARCH_64_BIT
      set_rd(mulhsu(rs1(), rs2()));
#else
      set_rd(sext32((sext32(rs1()) * reg_t((uint32_t)rs2())) >> 32));
#endif /*V8_TARGET_ARCH_64_BIT*/
      break;
    }
    case RO_MULHU: {
#ifdef V8_TARGET_ARCH_64_BIT
      set_rd(mulhu(rs1(), rs2()));
#else
      set_rd(sext32(((uint64_t)(uint32_t)rs1() * (uint64_t)(uint32_t)rs2()) >>
                    32));
#endif /*V8_TARGET_ARCH_64_BIT*/
      break;
    }
    case RO_DIV: {
      sreg_t lhs = sext_xlen(rs1());
      sreg_t rhs = sext_xlen(rs2());
      if (rhs == 0) {
        set_rd(UINT64_MAX);
      } else if (lhs == INT64_MIN && rhs == -1) {
        set_rd(lhs);
      } else {
        set_rd(sext_xlen(lhs / rhs));
      }
      break;
    }
    case RO_DIVU: {
      reg_t lhs = zext_xlen(rs1());
      reg_t rhs = zext_xlen(rs2());
      if (rhs == 0) {
        set_rd(UINT64_MAX);
      } else {
        set_rd(sext_xlen(lhs / rhs));
      }
      break;
    }
    case RO_REM: {
      sreg_t lhs = sext_xlen(rs1());
      sreg_t rhs = sext_xlen(rs2());
      if (rhs == 0) {
        set_rd(lhs);
      } else if (lhs == INT64_MIN && rhs == -1) {
        set_rd(0);
      } else {
        set_rd(sext_xlen(lhs % rhs));
      }
      break;
    }
    case RO_REMU: {
      reg_t lhs = zext_xlen(rs1());
      reg_t rhs = zext_xlen(rs2());
      if (rhs == 0) {
        set_rd(sext_xlen(rs1()));
      } else {
        set_rd(sext_xlen(lhs % rhs));
      }
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_MULW: {
      set_rd(sext32(rs1() * rs2()));
      break;
    }
    case RO_DIVW: {
      sreg_t lhs = sext32(rs1());
      sreg_t rhs = sext32(rs2());
      if (rhs == 0) {
        set_rd(UINT64_MAX);
      } else {
        set_rd(sext32(lhs / rhs));
      }
      break;
    }
    case RO_DIVUW: {
      reg_t lhs = zext32(rs1());
      reg_t rhs = zext32(rs2());
      if (rhs == 0) {
        set_rd(UINT64_MAX);
      } else {
        set_rd(sext32(lhs / rhs));
      }
      break;
    }
    case RO_REMW: {
      sreg_t lhs = sext32(rs1());
      sreg_t rhs = sext32(rs2());
      if (rhs == 0) {
        set_rd(lhs);
      } else {
        set_rd(sext32(lhs % rhs));
      }
      break;
    }
    case RO_REMUW: {
      reg_t lhs = zext32(rs1());
      reg_t rhs = zext32(rs2());
      if (rhs == 0) {
        set_rd(sext32(lhs));
      } else {
        set_rd(sext32(lhs % rhs));
      }
      break;
    }
#endif /*V8_TARGET_ARCH_64_BIT*/
       // TODO: End Add RISCV M extension macro
    default: {
      switch (instr_.BaseOpcode()) {
        case AMO:
          DecodeRVRAType();
          break;
        case OP_FP:
          DecodeRVRFPType();
          break;
        default:
          UNSUPPORTED();
      }
    }
  }
}

float Simulator::RoundF2FHelper(float input_val, int rmode) {
  if (rmode == DYN) rmode = get_dynamic_rounding_mode();

  float rounded = 0;
  switch (rmode) {
    case RNE: {  // Round to Nearest, tiest to Even
      int curr_mode = fegetround();
      fesetround(FE_TONEAREST);
      rounded = std::nearbyintf(input_val);
      fesetround(curr_mode);
      break;
    }
    case RTZ:  // Round towards Zero
      rounded = std::truncf(input_val);
      break;
    case RDN:  // Round Down (towards -infinity)
      rounded = std::floorf(input_val);
      break;
    case RUP:  // Round Up (towards +infinity)
      rounded = std::ceilf(input_val);
      break;
    case RMM:  // Round to Nearest, tiest to Max Magnitude
      rounded = std::roundf(input_val);
      break;
    default:
      UNREACHABLE();
  }

  return rounded;
}

double Simulator::RoundF2FHelper(double input_val, int rmode) {
  if (rmode == DYN) rmode = get_dynamic_rounding_mode();

  double rounded = 0;
  switch (rmode) {
    case RNE: {  // Round to Nearest, tiest to Even
      int curr_mode = fegetround();
      fesetround(FE_TONEAREST);
      rounded = std::nearbyint(input_val);
      fesetround(curr_mode);
      break;
    }
    case RTZ:  // Round towards Zero
      rounded = std::trunc(input_val);
      break;
    case RDN:  // Round Down (towards -infinity)
      rounded = std::floor(input_val);
      break;
    case RUP:  // Round Up (towards +infinity)
      rounded = std::ceil(input_val);
      break;
    case RMM:  // Round to Nearest, tiest to Max Magnitude
      rounded = std::round(input_val);
      break;
    default:
      UNREACHABLE();
  }
  return rounded;
}

// convert rounded floating-point to integer types, handle input values that are
// out-of-range, underflow, or NaN, and set appropriate fflags
template <typename I_TYPE, typename F_TYPE>
I_TYPE Simulator::RoundF2IHelper(F_TYPE original, int rmode) {
  DCHECK(std::is_integral<I_TYPE>::value);

  DCHECK((std::is_same<F_TYPE, float>::value ||
          std::is_same<F_TYPE, double>::value));

  I_TYPE max_i = std::numeric_limits<I_TYPE>::max();
  I_TYPE min_i = std::numeric_limits<I_TYPE>::min();

  if (!std::isfinite(original)) {
    set_fflags(kInvalidOperation);
    if (std::isnan(original) ||
        original == std::numeric_limits<F_TYPE>::infinity()) {
      return max_i;
    } else {
      DCHECK(original == -std::numeric_limits<F_TYPE>::infinity());
      return min_i;
    }
  }

  F_TYPE rounded = RoundF2FHelper(original, rmode);
  if (original != rounded) set_fflags(kInexact);

  if (!std::isfinite(rounded)) {
    set_fflags(kInvalidOperation);
    if (std::isnan(rounded) ||
        rounded == std::numeric_limits<F_TYPE>::infinity()) {
      return max_i;
    } else {
      DCHECK(rounded == -std::numeric_limits<F_TYPE>::infinity());
      return min_i;
    }
  }

  // FIXME (RISCV): comparison of rounded (float) and max_i (integer) may not be
  // precise because max_i is promoted to floating point during comparison.
  // Rounding up may happen when converting max_i to floating-point, e.g.,
  // max<uint64> is 9223372036854775807 vs. (double)max<uint64> is
  // 9223372036854775808.00000000000000

  // Since integer max values are either all 1s (for unsigned) or all 1s except
  // for sign-bit (for signed), they cannot be represented precisely in floating
  // point, in order to precisely tell whether the rounded floating point is
  // within the max range, we compare against (max_i+1) which would have a
  // single 1 w/ many trailing zeros
  float max_i_plus_1 =
      std::is_same<uint64_t, I_TYPE>::value
          ? 0x1p64f  // uint64_t::max + 1 cannot be represented in integers, so
                     // use its float representation directly
          : static_cast<float>(static_cast<uint64_t>(max_i) + 1);
  if (rounded >= max_i_plus_1) {
    set_fflags(kOverflow | kInvalidOperation);
    return max_i;
  }

  // Since min_i (either 0 for unsigned, or for signed) is represented precisely
  // in floating-point,  comparing rounded directly against min_i
  if (rounded <= min_i) {
    if (rounded < min_i) set_fflags(kOverflow | kInvalidOperation);
    return min_i;
  }

  F_TYPE underflow_fval =
      std::is_same<F_TYPE, float>::value ? FLT_MIN : DBL_MIN;
  if (rounded < underflow_fval && rounded > -underflow_fval && rounded != 0) {
    set_fflags(kUnderflow);
  }

  return static_cast<I_TYPE>(rounded);
}

#define BIT(n) (0x1LL << n)
#define QUIET_BIT_S(nan) (bit_cast<int32_t>(nan) & BIT(22))
#define QUIET_BIT_D(nan) (bit_cast<int64_t>(nan) & BIT(51))
static inline bool isSnan(float fp) { return !QUIET_BIT_S(fp); }
static inline bool isSnan(double fp) { return !QUIET_BIT_D(fp); }
#undef QUIET_BIT_S
#undef QUIET_BIT_D

template <typename T>
static int64_t FclassHelper(T value) {
  switch (std::fpclassify(value)) {
    case FP_INFINITE:
      return (std::signbit(value) ? kNegativeInfinity : kPositiveInfinity);
    case FP_NAN:
      return (isSnan(value) ? kSignalingNaN : kQuietNaN);
    case FP_NORMAL:
      return (std::signbit(value) ? kNegativeNormalNumber
                                  : kPositiveNormalNumber);
    case FP_SUBNORMAL:
      return (std::signbit(value) ? kNegativeSubnormalNumber
                                  : kPositiveSubnormalNumber);
    case FP_ZERO:
      return (std::signbit(value) ? kNegativeZero : kPositiveZero);
    default:
      UNREACHABLE();
  }
}

template <typename T>
bool Simulator::CompareFHelper(T input1, T input2, FPUCondition cc) {
  DCHECK(std::is_floating_point<T>::value);
  bool result = false;
  switch (cc) {
    case LT:
    case LE:
      if (std::isnan(input1) || std::isnan(input2)) {
        set_fflags(kInvalidOperation);
        result = false;
      } else {
        result = (cc == LT) ? (input1 < input2) : (input1 <= input2);
      }
      break;

    case EQ:
      if (std::numeric_limits<T>::signaling_NaN() == input1 ||
          std::numeric_limits<T>::signaling_NaN() == input2) {
        set_fflags(kInvalidOperation);
      }
      if (std::isnan(input1) || std::isnan(input2)) {
        result = false;
      } else {
        result = (input1 == input2);
      }
      break;

    default:
      UNREACHABLE();
  }
  return result;
}

void Simulator::DecodeRVRAType() {
  // TODO: Add macro for RISCV A extension
  // Special handling for A extension instructions because it uses func5
  // For all A extension instruction, V8 simulator is pure sequential. No
  // Memory address lock or other synchronizaiton behaviors.
  switch (instr_.InstructionBits() & kRATypeMask) {
    case RO_LR_W: {
      int64_t addr = rs1();
      auto val = RV_ReadMem<int32_t>(addr, instr_.instr());
      set_rd(sext32(val), false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
    case RO_SC_W: {
      RV_WriteMem<int32_t>(rs1(), (int32_t)rs2(), instr_.instr());
      set_rd(0, false);  // Always success in simulator
      break;
    }
    case RO_AMOSWAP_W: {
      set_rd(sext32(RV_amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return (uint32_t)rs2(); }, instr_.instr(),
          WORD)));
      break;
    }
    case RO_AMOADD_W: {
      set_rd(sext32(RV_amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs + (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOXOR_W: {
      set_rd(sext32(RV_amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs ^ (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOAND_W: {
      set_rd(sext32(RV_amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs & (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOOR_W: {
      set_rd(sext32(RV_amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs | (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMIN_W: {
      set_rd(sext32(RV_amo<int32_t>(
          rs1(), [&](int32_t lhs) { return std::min(lhs, (int32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMAX_W: {
      set_rd(sext32(RV_amo<int32_t>(
          rs1(), [&](int32_t lhs) { return std::max(lhs, (int32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMINU_W: {
      set_rd(sext32(RV_amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return std::min(lhs, (uint32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMAXU_W: {
      set_rd(sext32(RV_amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return std::max(lhs, (uint32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_LR_D: {
      int64_t addr = rs1();
      auto val = RV_ReadMem<int64_t>(addr, instr_.instr());
      set_rd(val, false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
    case RO_SC_D: {
      RV_WriteMem<int64_t>(rs1(), rs2(), instr_.instr());
      set_rd(0, false);  // Always success in simulator
      break;
    }
    case RO_AMOSWAP_D: {
      set_rd(RV_amo<int64_t>(
          rs1(), [&](int64_t lhs) { return rs2(); }, instr_.instr(), DWORD));
      break;
    }
    case RO_AMOADD_D: {
      set_rd(RV_amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs + rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOXOR_D: {
      set_rd(RV_amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs ^ rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOAND_D: {
      set_rd(RV_amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs & rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOOR_D: {
      set_rd(RV_amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs | rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOMIN_D: {
      set_rd(RV_amo<int64_t>(
          rs1(), [&](int64_t lhs) { return std::min(lhs, rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMAX_D: {
      set_rd(RV_amo<int64_t>(
          rs1(), [&](int64_t lhs) { return std::max(lhs, rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMINU_D: {
      set_rd(RV_amo<uint64_t>(
          rs1(), [&](uint64_t lhs) { return std::min(lhs, (uint64_t)rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMAXU_D: {
      set_rd(RV_amo<uint64_t>(
          rs1(), [&](uint64_t lhs) { return std::max(lhs, (uint64_t)rs2()); },
          instr_.instr(), DWORD));
      break;
    }
#endif /*V8_TARGET_ARCH_64_BIT*/
    // TODO: End Add macro for RISCV A extension
    default: {
      UNSUPPORTED();
    }
  }
}

void Simulator::DecodeRVRFPType() {
  // OP_FP instructions (F/D) uses func7 first. Some further uses fun3 and rs2()

  // kRATypeMask is only for func7
  switch (instr_.InstructionBits() & kRFPTypeMask) {
    // TODO: Add macro for RISCV F extension
    case RO_FADD_S: {
      // TODO: use rm value (round mode)
      auto fn = [](float frs1, float frs2) { return frs1 + frs2; };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FSUB_S: {
      // TODO: use rm value (round mode)
      auto fn = [](float frs1, float frs2) { return frs1 - frs2; };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FMUL_S: {
      // TODO: use rm value (round mode)
      auto fn = [](float frs1, float frs2) { return frs1 * frs2; };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FDIV_S: {
      // TODO: use rm value (round mode)
      auto fn = [](float frs1, float frs2) { return frs1 / frs2; };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FSQRT_S: {
      if (instr_.Rs2Value() == 0b00000) {
        // TODO: use rm value (round mode)
        auto fn = [](float frs) { return std::sqrt(frs); };
        set_frd(CanonicalizeFPUOp1<float>(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FSGNJ_S: {  // RO_FSGNJN_S  RO_FSQNJX_S
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FSGNJ_S
          set_frd(fsgnj32(frs1(), frs2(), false, false));
          break;
        }
        case 0b001: {  // RO_FSGNJN_S
          set_frd(fsgnj32(frs1(), frs2(), true, false));
          break;
        }
        case 0b010: {  // RO_FSQNJX_S
          set_frd(fsgnj32(frs1(), frs2(), false, true));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMIN_S: {  // RO_FMAX_S
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FMIN_S
          set_frd(FMaxMinHelper(frs1(), frs2(), MaxMinKind::kMin));
          break;
        }
        case 0b001: {  // RO_FMAX_S
          set_frd(FMaxMinHelper(frs1(), frs2(), MaxMinKind::kMax));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_W_S: {  // RO_FCVT_WU_S , 64F RO_FCVT_L_S RO_FCVT_LU_S
      float original_val = frs1();
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_W_S
          set_rd(RoundF2IHelper<int32_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00001: {  // RO_FCVT_WU_S
          set_rd(RoundF2IHelper<uint32_t>(original_val, instr_.RoundMode()));
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b00010: {  // RO_FCVT_L_S
          set_rd(RoundF2IHelper<int64_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00011: {  // RO_FCVT_LU_S
          set_rd(RoundF2IHelper<uint64_t>(original_val, instr_.RoundMode()));
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMV: {  // RO_FCLASS_S
      switch (instr_.Funct3Value()) {
        case 0b000: {
          if (instr_.Rs2Value() == 0b00000) {
            // RO_FMV_X_W
            set_rd(bit_cast<int32_t>(frs1()));
          } else {
            UNSUPPORTED();
          }
          break;
        }
        case 0b001: {  // RO_FCLASS_S
          set_rd(FclassHelper(frs1()));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    // FIXME (RISCV): implement handling of NaN (quiet and signalling)
    case RO_FLE_S: {  // RO_FEQ_S RO_FLT_S RO_FLE_S
      switch (instr_.Funct3Value()) {
        case 0b010: {  // RO_FEQ_S
          set_rd(CompareFHelper(frs1(), frs2(), EQ));
          break;
        }
        case 0b001: {  // RO_FLT_S
          set_rd(CompareFHelper(frs1(), frs2(), LT));
          break;
        }
        case 0b000: {  // RO_FLE_S
          set_rd(CompareFHelper(frs1(), frs2(), LE));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_S_W: {  // RO_FCVT_S_WU , 64F RO_FCVT_S_L RO_FCVT_S_LU
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_S_W
          set_frd((float)(int32_t)rs1());
          break;
        }
        case 0b00001: {  // RO_FCVT_S_WU
          set_frd((float)(uint32_t)rs1());
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b00010: {  // RO_FCVT_S_L
          set_frd((float)(int64_t)rs1());
          break;
        }
        case 0b00011: {  // RO_FCVT_S_LU
          set_frd((float)(uint64_t)rs1());
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMV_W_X: {
      if (instr_.Funct3Value() == 0b000) {
        // since FMV preserves source bit-pattern, no need to canonize
        set_frd(bit_cast<float>((uint32_t)rs1()));
      } else {
        UNSUPPORTED();
      }
      break;
    }
      // TODO: Add macro for RISCV D extension
    case RO_FADD_D: {
      // TODO: use rm value (round mode)
      auto fn = [](double drs1, double drs2) { return drs1 + drs2; };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FSUB_D: {
      // TODO: use rm value (round mode)
      auto fn = [](double drs1, double drs2) { return drs1 - drs2; };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FMUL_D: {
      // TODO: use rm value (round mode)
      auto fn = [](double drs1, double drs2) { return drs1 * drs2; };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FDIV_D: {
      // TODO: use rm value (round mode)
      auto fn = [](double drs1, double drs2) { return drs1 / drs2; };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FSQRT_D: {
      if (instr_.Rs2Value() == 0b00000) {
        // TODO: use rm value (round mode)
        auto fn = [](double drs) { return std::sqrt(drs); };
        set_drd(CanonicalizeFPUOp1<double>(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FSGNJ_D: {  // RO_FSGNJN_D RO_FSQNJX_D
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FSGNJ_D
          set_drd(fsgnj64(drs1(), drs2(), false, false));
          break;
        }
        case 0b001: {  // RO_FSGNJN_D
          set_drd(fsgnj64(drs1(), drs2(), true, false));
          break;
        }
        case 0b010: {  // RO_FSQNJX_D
          set_drd(fsgnj64(drs1(), drs2(), false, true));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMIN_D: {  // RO_FMAX_D
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FMIN_D
          set_drd(FMaxMinHelper(drs1(), drs2(), MaxMinKind::kMin));
          break;
        }
        case 0b001: {  // RO_FMAX_D
          set_drd(FMaxMinHelper(drs1(), drs2(), MaxMinKind::kMax));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case (RO_FCVT_S_D & kRFPTypeMask): {
      if (instr_.Rs2Value() == 0b00001) {
        auto fn = [](double drs) { return (float)drs; };
        set_frd(CanonicalizeDoubleToFloatOperation(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FCVT_D_S: {
      if (instr_.Rs2Value() == 0b00000) {
        auto fn = [](float frs) { return (double)frs; };
        set_drd(CanonicalizeFloatToDoubleOperation(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    // FIXME (RISCV): implement handling of NaN (quiet and signalling)
    case RO_FLE_D: {  // RO_FEQ_D RO_FLT_D RO_FLE_D
      switch (instr_.Funct3Value()) {
        case 0b010: {  // RO_FEQ_S
          set_rd(CompareFHelper(drs1(), drs2(), EQ));
          break;
        }
        case 0b001: {  // RO_FLT_D
          set_rd(CompareFHelper(drs1(), drs2(), LT));
          break;
        }
        case 0b000: {  // RO_FLE_D
          set_rd(CompareFHelper(drs1(), drs2(), LE));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case (RO_FCLASS_D & kRFPTypeMask): {  // RO_FCLASS_D , 64D RO_FMV_X_D
      if (instr_.Rs2Value() != 0b00000) {
        UNSUPPORTED();
        break;
      }
      switch (instr_.Funct3Value()) {
        case 0b001: {  // RO_FCLASS_D
          set_rd(FclassHelper(drs1()));
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b000: {  // RO_FMV_X_D
          set_rd(bit_cast<int64_t>(drs1()));
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_W_D: {  // RO_FCVT_WU_D , 64F RO_FCVT_L_D RO_FCVT_LU_D
      double original_val = drs1();
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_W_D
          set_rd(RoundF2IHelper<int32_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00001: {  // RO_FCVT_WU_D
          set_rd(RoundF2IHelper<uint32_t>(original_val, instr_.RoundMode()));
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b00010: {  // RO_FCVT_L_D
          set_rd(RoundF2IHelper<int64_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00011: {  // RO_FCVT_LU_D
          set_rd(RoundF2IHelper<uint64_t>(original_val, instr_.RoundMode()));
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_D_W: {  // RO_FCVT_D_WU , 64F RO_FCVT_D_L RO_FCVT_D_LU
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_D_W
          set_drd((int32_t)rs1());
          break;
        }
        case 0b00001: {  // RO_FCVT_D_WU
          set_drd((uint32_t)rs1());
          break;
        }
#ifdef V8_TARGET_ARCH_64_BIT
        case 0b00010: {  // RO_FCVT_D_L
          set_drd((int64_t)rs1());
          break;
        }
        case 0b00011: {  // RO_FCVT_D_LU
          set_drd((uint64_t)rs1());
          break;
        }
#endif /* V8_TARGET_ARCH_64_BIT */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_FMV_D_X: {
      if (instr_.Funct3Value() == 0b000 && instr_.Rs2Value() == 0b00000) {
        // Since FMV preserves source bit-pattern, no need to canonize
        set_drd(bit_cast<double>(rs1()));
      } else {
        UNSUPPORTED();
      }
      break;
    }
#endif /* V8_TARGET_ARCH_64_BIT */
    default: {
      UNSUPPORTED();
    }
  }
}

void Simulator::DecodeRVR4Type() {
  switch (instr_.InstructionBits() & kR4TypeMask) {
    // TODO: use F Extension macro block
    case RO_FMADD_S: {
      // TODO: use rm value (round mode)
      auto fn = [](float frs1, float frs2, float frs3) {
        return frs1 * frs2 + frs3;
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FMSUB_S: {
      // TODO: use rm value (round mode)
      auto fn = [](float frs1, float frs2, float frs3) {
        return frs1 * frs2 - frs3;
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FNMSUB_S: {
      // TODO: use rm value (round mode)
      auto fn = [](float frs1, float frs2, float frs3) {
        return -(frs1 * frs2) + frs3;
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FNMADD_S: {
      // TODO: use rm value (round mode)
      auto fn = [](float frs1, float frs2, float frs3) {
        return -(frs1 * frs2) - frs3;
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    // TODO: use F Extension macro block
    case RO_FMADD_D: {
      // TODO: use rm value (round mode)
      auto fn = [](double drs1, double drs2, double drs3) {
        return drs1 * drs2 + drs3;
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FMSUB_D: {
      // TODO: use rm value (round mode)
      auto fn = [](double drs1, double drs2, double drs3) {
        return drs1 * drs2 - drs3;
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FNMSUB_D: {
      // TODO: use rm value (round mode)
      auto fn = [](double drs1, double drs2, double drs3) {
        return -(drs1 * drs2) + drs3;
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FNMADD_D: {
      // TODO: use rm value (round mode)
      auto fn = [](double drs1, double drs2, double drs3) {
        return -(drs1 * drs2) - drs3;
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeRVIType() {
  switch (instr_.InstructionBits() & kITypeMask) {
    case RO_JALR: {
      set_rd(get_pc() + kInstrSize);
      // Note: No need to shift 2 for JALR's imm12, but set lowest bit to 0.
      int64_t next_pc = (rs1() + imm12()) & ~reg_t(1);
      set_pc(next_pc);
      break;
    }
    case RO_LB: {
      int64_t addr = rs1() + imm12();
      int8_t val = RV_ReadMem<int8_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
    case RO_LH: {
      int64_t addr = rs1() + imm12();
      int16_t val = RV_ReadMem<int16_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
    case RO_LW: {
      int64_t addr = rs1() + imm12();
      int32_t val = RV_ReadMem<int32_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
    case RO_LBU: {
      int64_t addr = rs1() + imm12();
      uint8_t val = RV_ReadMem<uint8_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
    case RO_LHU: {
      int64_t addr = rs1() + imm12();
      uint16_t val = RV_ReadMem<uint16_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_LWU: {
      int64_t addr = rs1() + imm12();
      uint32_t val = RV_ReadMem<uint32_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
    case RO_LD: {
      int64_t addr = rs1() + imm12();
      int64_t val = RV_ReadMem<int64_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, get_register(RV_rd_reg()));
      break;
    }
#endif /*V8_TARGET_ARCH_64_BIT*/
    case RO_ADDI: {
      set_rd(sext_xlen(rs1() + imm12()));
      break;
    }
    case RO_SLTI: {
      set_rd(sreg_t(rs1()) < sreg_t(imm12()));
      break;
    }
    case RO_SLTIU: {
      set_rd(reg_t(rs1()) < reg_t(imm12()));
      break;
    }
    case RO_XORI: {
      set_rd(imm12() ^ rs1());
      break;
    }
    case RO_ORI: {
      set_rd(imm12() | rs1());
      break;
    }
    case RO_ANDI: {
      set_rd(imm12() & rs1());
      break;
    }
    case RO_SLLI: {
      require(shamt() < xlen);
      set_rd(sext_xlen(rs1() << shamt()));
      break;
    }
    case RO_SRLI: {  //  RO_SRAI
      if (!instr_.IsArithShift()) {
        require(shamt() < xlen);
        set_rd(sext_xlen(zext_xlen(rs1()) >> shamt()));
      } else {
        require(shamt() < xlen);
        set_rd(sext_xlen(sext_xlen(rs1()) >> shamt()));
      }
      break;
    }
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_ADDIW: {
      set_rd(sext32(rs1() + imm12()));
      break;
    }
    case RO_SLLIW: {
      set_rd(sext32(rs1() << shamt()));
      break;
    }
    case RO_SRLIW: {  //  RO_SRAIW
      if (!instr_.IsArithShift()) {
        set_rd(sext32(uint32_t(rs1()) >> shamt()));
      } else {
        set_rd(sext32(int32_t(rs1()) >> shamt()));
      }
      break;
    }
#endif /*V8_TARGET_ARCH_64_BIT*/
    case RO_FENCE: {
      // DO nothing in sumulator
      break;
    }
    case RO_ECALL: {                   // RO_EBREAK
      if (instr_.Imm12Value() == 0) {  // ECALL
        SoftwareInterrupt();
      } else if (instr_.Imm12Value() == 1) {  // EBREAK
        SoftwareInterrupt();
      } else {
        UNSUPPORTED();
      }
      break;
    }
      // TODO: use Zifencei Standard Extension macro block
    case RO_FENCE_I: {
      // spike: flush icache.
      break;
    }
      // TODO: use Zicsr Standard Extension macro block
    case RO_CSRRW: {
      if (RV_rd_reg() != zero_reg) {
        set_rd(zext_xlen(read_csr_value(csr_reg())));
      }
      write_csr_value(csr_reg(), rs1());
      break;
    }
    case RO_CSRRS: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (rs1_reg() != zero_reg) {
        set_csr_bits(csr_reg(), rs1());
      }
      break;
    }
    case RO_CSRRC: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (rs1_reg() != zero_reg) {
        clear_csr_bits(csr_reg(), rs1());
      }
      break;
    }
    case RO_CSRRWI: {
      if (RV_rd_reg() != zero_reg) {
        set_rd(zext_xlen(read_csr_value(csr_reg())));
      }
      write_csr_value(csr_reg(), imm5CSR());
      break;
    }
    case RO_CSRRSI: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (imm5CSR() != 0) {
        set_csr_bits(csr_reg(), imm5CSR());
      }
      break;
    }
    case RO_CSRRCI: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (imm5CSR() != 0) {
        clear_csr_bits(csr_reg(), imm5CSR());
      }
      break;
    }
    // TODO: use F Extension macro block
    case RO_FLW: {
      int64_t addr = rs1() + imm12();
      float val = RV_ReadMem<float>(addr, instr_.instr());
      set_frd(val, false);
      TraceMemRd(addr, val, get_fpu_register(RV_frd_reg()));
      break;
    }
    // TODO: use D Extension macro block
    case RO_FLD: {
      int64_t addr = rs1() + imm12();
      double val = RV_ReadMem<double>(addr, instr_.instr());
      set_drd(val, false);
      TraceMemRd(addr, val, get_fpu_register(RV_frd_reg()));
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeRVSType() {
  switch (instr_.InstructionBits() & kSTypeMask) {
    case RO_SB:
      RV_WriteMem<uint8_t>(rs1() + s_imm12(), (uint8_t)rs2(), instr_.instr());
      break;
    case RO_SH:
      RV_WriteMem<uint16_t>(rs1() + s_imm12(), (uint16_t)rs2(), instr_.instr());
      break;
    case RO_SW:
      RV_WriteMem<uint32_t>(rs1() + s_imm12(), (uint32_t)rs2(), instr_.instr());
      break;
#ifdef V8_TARGET_ARCH_64_BIT
    case RO_SD:
      RV_WriteMem<uint64_t>(rs1() + s_imm12(), (uint64_t)rs2(), instr_.instr());
      break;
#endif /*V8_TARGET_ARCH_64_BIT*/
    // TODO: use F Extension macro block
    case RO_FSW: {
      RV_WriteMem<float>(rs1() + s_imm12(), frs2(), instr_.instr());
      break;
    }
    // TODO: use D Extension macro block
    case RO_FSD: {
      RV_WriteMem<double>(rs1() + s_imm12(), drs2(), instr_.instr());
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeRVBType() {
  switch (instr_.InstructionBits() & kBTypeMask) {
    case RO_BEQ:
      if (rs1() == rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BNE:
      if (rs1() != rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BLT:
      if (rs1() < rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BGE:
      if (rs1() >= rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BLTU:
      if ((reg_t)rs1() < (reg_t)rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BGEU:
      if ((reg_t)rs1() >= (reg_t)rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    default:
      UNSUPPORTED();
  }
}
void Simulator::DecodeRVUType() {
  // U Type doesn't have additoinal mask
  switch (instr_.BaseOpcodeFieldRaw()) {
    case RO_LUI:
      set_rd(u_imm());
      break;
    case RO_AUIPC:
      set_rd(sext_xlen(u_imm() + get_pc()));
      break;
    default:
      UNSUPPORTED();
  }
}
void Simulator::DecodeRVJType() {
  // J Type doesn't have additional mask
  switch (instr_.BaseOpcodeValue()) {
    case RO_JAL: {
      set_rd(get_pc() + kInstrSize);
      int64_t next_pc = get_pc() + imm20J();
      set_pc(next_pc);
      break;
    }
    default:
      UNSUPPORTED();
  }
}

// Executes the current instruction.
void Simulator::InstructionDecode(Instruction* instr) {
  if (v8::internal::FLAG_check_icache) {
    CheckICache(i_cache(), instr);
  }
  pc_modified_ = false;

  v8::internal::EmbeddedVector<char, 256> buffer;

  if (::v8::internal::FLAG_trace_sim) {
    SNPrintF(trace_buf_, " ");
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    // Use a reasonably large buffer.
    dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(instr));
  }

  instr_ = instr;
  switch (instr_.InstructionType()) {
    case Instruction::kRType:
      DecodeRVRType();
      break;
    case Instruction::kR4Type:
      DecodeRVR4Type();
      break;
    case Instruction::kIType:
      DecodeRVIType();
      break;
    case Instruction::kSType:
      DecodeRVSType();
      break;
    case Instruction::kBType:
      DecodeRVBType();
      break;
    case Instruction::kUType:
      DecodeRVUType();
      break;
    case Instruction::kJType:
      DecodeRVJType();
      break;
    default:
      if (::v8::internal::FLAG_trace_sim) {
        std::cout << "Unrecognized instruction [@pc=0x" << std::hex
                  << registers_[pc] << "]: 0x" << instr->InstructionBits()
                  << std::endl;
      }
      UNSUPPORTED();
  }

  if (::v8::internal::FLAG_trace_sim) {
    PrintF("  0x%08" PRIxPTR "   %-44s   %s\n",
           reinterpret_cast<intptr_t>(instr), buffer.begin(),
           trace_buf_.begin());
  }

  if (!pc_modified_) {
    set_register(pc, reinterpret_cast<int64_t>(instr) + kInstrSize);
  }
}

void Simulator::Execute() {
  // Get the PC to simulate. Cannot use the accessor here as we need the
  // raw PC value and not the one used as input to arithmetic instructions.
  int64_t program_counter = get_pc();
  if (::v8::internal::FLAG_stop_sim_at == 0) {
    // Fast version of the dispatch loop without checking whether the
    // simulator should be stopping at a particular executed instruction.
    while (program_counter != end_sim_pc) {
      Instruction* instr = reinterpret_cast<Instruction*>(program_counter);
      icount_++;
      InstructionDecode(instr);
      program_counter = get_pc();
    }
  } else {
    // FLAG_stop_sim_at is at the non-default value. Stop in the debugger when
    // we reach the particular instruction count.
    while (program_counter != end_sim_pc) {
      Instruction* instr = reinterpret_cast<Instruction*>(program_counter);
      icount_++;
      if (icount_ == static_cast<int64_t>(::v8::internal::FLAG_stop_sim_at)) {
        RiscvDebugger dbg(this);
        dbg.Debug();
      } else {
        InstructionDecode(instr);
      }
      program_counter = get_pc();
    }
  }
}

void Simulator::CallInternal(Address entry) {
  // Adjust JS-based stack limit to C-based stack limit.
  isolate_->stack_guard()->AdjustStackLimitForSimulator();

  // Prepare to execute the code at entry.
  set_register(pc, static_cast<int64_t>(entry));
  // Put down marker for end of simulation. The simulator will stop simulation
  // when the PC reaches this value. By saving the "end simulation" value into
  // the LR the simulation stops when returning to this call point.
  set_register(ra, end_sim_pc);

  // Remember the values of callee-saved registers.
  // The code below assumes that r9 is not used as sb (static base) in
  // simulator code and therefore is regarded as a callee-saved register.
  int64_t s0_val = get_register(s0);
  int64_t s1_val = get_register(s1);
  int64_t s2_val = get_register(s2);
  int64_t s3_val = get_register(s3);
  int64_t s4_val = get_register(s4);
  int64_t s5_val = get_register(s5);
  int64_t s6_val = get_register(s6);
  int64_t s7_val = get_register(s7);
  int64_t gp_val = get_register(gp);
  int64_t sp_val = get_register(sp);
  int64_t fp_val = get_register(fp);

  // Set up the callee-saved registers with a known value. To be able to check
  // that they are preserved properly across JS execution.
  int64_t callee_saved_value = icount_;
  set_register(s0, callee_saved_value);
  set_register(s1, callee_saved_value);
  set_register(s2, callee_saved_value);
  set_register(s3, callee_saved_value);
  set_register(s4, callee_saved_value);
  set_register(s5, callee_saved_value);
  set_register(s6, callee_saved_value);
  set_register(s7, callee_saved_value);
  set_register(gp, callee_saved_value);
  set_register(fp, callee_saved_value);

  // Start the simulation.
  Execute();

  // Check that the callee-saved registers have been preserved.
  CHECK_EQ(callee_saved_value, get_register(s0));
  CHECK_EQ(callee_saved_value, get_register(s1));
  CHECK_EQ(callee_saved_value, get_register(s2));
  CHECK_EQ(callee_saved_value, get_register(s3));
  CHECK_EQ(callee_saved_value, get_register(s4));
  CHECK_EQ(callee_saved_value, get_register(s5));
  CHECK_EQ(callee_saved_value, get_register(s6));
  CHECK_EQ(callee_saved_value, get_register(s7));
  CHECK_EQ(callee_saved_value, get_register(gp));
  CHECK_EQ(callee_saved_value, get_register(fp));

  // Restore callee-saved registers with the original value.
  set_register(s0, s0_val);
  set_register(s1, s1_val);
  set_register(s2, s2_val);
  set_register(s3, s3_val);
  set_register(s4, s4_val);
  set_register(s5, s5_val);
  set_register(s6, s6_val);
  set_register(s7, s7_val);
  set_register(gp, gp_val);
  set_register(sp, sp_val);
  set_register(fp, fp_val);
}

intptr_t Simulator::CallImpl(Address entry, int argument_count,
                             const intptr_t* arguments) {
  constexpr int kRegisterPassedArguments = 8;
  // Set up arguments.

  // First four arguments passed in registers in both ABI's.
  int reg_arg_count = std::min(kRegisterPassedArguments, argument_count);
  if (reg_arg_count > 0) set_register(a0, arguments[0]);
  if (reg_arg_count > 1) set_register(a1, arguments[1]);
  if (reg_arg_count > 2) set_register(a2, arguments[2]);
  if (reg_arg_count > 3) set_register(a3, arguments[3]);

  // Up to eight arguments passed in registers in N64 ABI.
  // TODO(plind): N64 ABI calls these regs a4 - a7. Clarify this.
  if (reg_arg_count > 4) set_register(a4, arguments[4]);
  if (reg_arg_count > 5) set_register(a5, arguments[5]);
  if (reg_arg_count > 6) set_register(a6, arguments[6]);
  if (reg_arg_count > 7) set_register(a7, arguments[7]);

  if (::v8::internal::FLAG_trace_sim) {
    std::cout << "CallImpl: reg_arg_count = " << reg_arg_count << std::hex
              << " entry-pc (JSEntry) = 0x" << entry << " a0 (Isolate) = 0x"
              << get_register(a0) << " a1 (orig_func/new_target) = 0x"
              << get_register(a1) << " a2 (func/target) = 0x"
              << get_register(a2) << " a3 (receiver) = 0x" << get_register(a3)
              << " a4 (argc) = 0x" << get_register(a4) << std::endl;
  }

  // Remaining arguments passed on stack.
  int64_t original_stack = get_register(sp);
  // Compute position of stack on entry to generated code.
  int stack_args_count = argument_count - reg_arg_count;
  int stack_args_size = stack_args_count * sizeof(*arguments) + kCArgsSlotsSize;
  int64_t entry_stack = original_stack - stack_args_size;

  if (base::OS::ActivationFrameAlignment() != 0) {
    entry_stack &= -base::OS::ActivationFrameAlignment();
  }
  // Store remaining arguments on stack, from low to high memory.
  intptr_t* stack_argument = reinterpret_cast<intptr_t*>(entry_stack);
  memcpy(stack_argument + kCArgSlotCount, arguments + reg_arg_count,
         stack_args_count * sizeof(*arguments));
  set_register(sp, entry_stack);

  CallInternal(entry);

  // Pop stack passed arguments.
  CHECK_EQ(entry_stack, get_register(sp));
  set_register(sp, original_stack);

  // return get_register(a0);
  // RISCV uses a0 to return result
  return get_register(a0);
}

double Simulator::CallFP(Address entry, double d0, double d1) {
  if (!IsMipsSoftFloatABI) {
    const FPURegister fparg2 = fa1;
    set_fpu_register_double(fa0, d0);
    set_fpu_register_double(fparg2, d1);
  } else {
    int buffer[2];
    DCHECK(sizeof(buffer[0]) * 2 == sizeof(d0));
    memcpy(buffer, &d0, sizeof(d0));
    set_dw_register(a0, buffer);
    memcpy(buffer, &d1, sizeof(d1));
    set_dw_register(a2, buffer);
  }
  CallInternal(entry);
  if (!IsMipsSoftFloatABI) {
    return get_fpu_register_double(ft0);
  } else {
    return get_double_from_register_pair(a0);
  }
}

uintptr_t Simulator::PushAddress(uintptr_t address) {
  int64_t new_sp = get_register(sp) - sizeof(uintptr_t);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(new_sp);
  *stack_slot = address;
  set_register(sp, new_sp);
  return new_sp;
}

uintptr_t Simulator::PopAddress() {
  int64_t current_sp = get_register(sp);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(current_sp);
  uintptr_t address = *stack_slot;
  set_register(sp, current_sp + sizeof(uintptr_t));
  return address;
}

Simulator::LocalMonitor::LocalMonitor()
    : access_state_(MonitorAccess::Open),
      tagged_addr_(0),
      size_(TransactionSize::None) {}

void Simulator::LocalMonitor::Clear() {
  access_state_ = MonitorAccess::Open;
  tagged_addr_ = 0;
  size_ = TransactionSize::None;
}

void Simulator::LocalMonitor::NotifyLoad() {
  if (access_state_ == MonitorAccess::RMW) {
    // A non linked load could clear the local monitor. As a result, it's
    // most strict to unconditionally clear the local monitor on load.
    Clear();
  }
}

void Simulator::LocalMonitor::NotifyLoadLinked(uintptr_t addr,
                                               TransactionSize size) {
  access_state_ = MonitorAccess::RMW;
  tagged_addr_ = addr;
  size_ = size;
}

void Simulator::LocalMonitor::NotifyStore() {
  if (access_state_ == MonitorAccess::RMW) {
    // A non exclusive store could clear the local monitor. As a result, it's
    // most strict to unconditionally clear the local monitor on store.
    Clear();
  }
}

bool Simulator::LocalMonitor::NotifyStoreConditional(uintptr_t addr,
                                                     TransactionSize size) {
  if (access_state_ == MonitorAccess::RMW) {
    if (addr == tagged_addr_ && size_ == size) {
      Clear();
      return true;
    } else {
      return false;
    }
  } else {
    DCHECK(access_state_ == MonitorAccess::Open);
    return false;
  }
}

Simulator::GlobalMonitor::LinkedAddress::LinkedAddress()
    : access_state_(MonitorAccess::Open),
      tagged_addr_(0),
      next_(nullptr),
      prev_(nullptr),
      failure_counter_(0) {}

void Simulator::GlobalMonitor::LinkedAddress::Clear_Locked() {
  access_state_ = MonitorAccess::Open;
  tagged_addr_ = 0;
}

void Simulator::GlobalMonitor::LinkedAddress::NotifyLoadLinked_Locked(
    uintptr_t addr) {
  access_state_ = MonitorAccess::RMW;
  tagged_addr_ = addr;
}

void Simulator::GlobalMonitor::LinkedAddress::NotifyStore_Locked() {
  if (access_state_ == MonitorAccess::RMW) {
    // A non exclusive store could clear the global monitor. As a result, it's
    // most strict to unconditionally clear global monitors on store.
    Clear_Locked();
  }
}

bool Simulator::GlobalMonitor::LinkedAddress::NotifyStoreConditional_Locked(
    uintptr_t addr, bool is_requesting_thread) {
  if (access_state_ == MonitorAccess::RMW) {
    if (is_requesting_thread) {
      if (addr == tagged_addr_) {
        Clear_Locked();
        // Introduce occasional sc/scd failures. This is to simulate the
        // behavior of hardware, which can randomly fail due to background
        // cache evictions.
        if (failure_counter_++ >= kMaxFailureCounter) {
          failure_counter_ = 0;
          return false;
        } else {
          return true;
        }
      }
    } else if ((addr & kExclusiveTaggedAddrMask) ==
               (tagged_addr_ & kExclusiveTaggedAddrMask)) {
      // Check the masked addresses when responding to a successful lock by
      // another thread so the implementation is more conservative (i.e. the
      // granularity of locking is as large as possible.)
      Clear_Locked();
      return false;
    }
  }
  return false;
}

void Simulator::GlobalMonitor::NotifyLoadLinked_Locked(
    uintptr_t addr, LinkedAddress* linked_address) {
  linked_address->NotifyLoadLinked_Locked(addr);
  PrependProcessor_Locked(linked_address);
}

void Simulator::GlobalMonitor::NotifyStore_Locked(
    LinkedAddress* linked_address) {
  // Notify each thread of the store operation.
  for (LinkedAddress* iter = head_; iter; iter = iter->next_) {
    iter->NotifyStore_Locked();
  }
}

bool Simulator::GlobalMonitor::NotifyStoreConditional_Locked(
    uintptr_t addr, LinkedAddress* linked_address) {
  DCHECK(IsProcessorInLinkedList_Locked(linked_address));
  if (linked_address->NotifyStoreConditional_Locked(addr, true)) {
    // Notify the other processors that this StoreConditional succeeded.
    for (LinkedAddress* iter = head_; iter; iter = iter->next_) {
      if (iter != linked_address) {
        iter->NotifyStoreConditional_Locked(addr, false);
      }
    }
    return true;
  } else {
    return false;
  }
}

bool Simulator::GlobalMonitor::IsProcessorInLinkedList_Locked(
    LinkedAddress* linked_address) const {
  return head_ == linked_address || linked_address->next_ ||
         linked_address->prev_;
}

void Simulator::GlobalMonitor::PrependProcessor_Locked(
    LinkedAddress* linked_address) {
  if (IsProcessorInLinkedList_Locked(linked_address)) {
    return;
  }

  if (head_) {
    head_->prev_ = linked_address;
  }
  linked_address->prev_ = nullptr;
  linked_address->next_ = head_;
  head_ = linked_address;
}

void Simulator::GlobalMonitor::RemoveLinkedAddress(
    LinkedAddress* linked_address) {
  base::MutexGuard lock_guard(&mutex);
  if (!IsProcessorInLinkedList_Locked(linked_address)) {
    return;
  }

  if (linked_address->prev_) {
    linked_address->prev_->next_ = linked_address->next_;
  } else {
    head_ = linked_address->next_;
  }
  if (linked_address->next_) {
    linked_address->next_->prev_ = linked_address->prev_;
  }
  linked_address->prev_ = nullptr;
  linked_address->next_ = nullptr;
}

#undef SScanF

}  // namespace internal
}  // namespace v8

#endif  // USE_SIMULATOR