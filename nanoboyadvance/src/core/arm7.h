/*
* Copyright (C) 2015 Frederic Meyer
*
* This file is part of nanoboyadvance.
*
* nanoboyadvance is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 2 of the License, or
* (at your option) any later version.
*
* nanoboyadvance is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with nanoboyadvance. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <iostream>
#include <sstream>
#include "common/types.h"
#include "common/log.h"
#include "gba_memory.h"

#define ARM7_FASTHAX
#define arm_pack_instr(i) ((i) & 0xFFF) | (((i) & 0x0FF00000) >> 8)

// Macro for easier register access
#define reg(r) *gprs[r]

static const int ARM_CALLBACK_EXECUTE = 0;
static const int ARM_CALLBACK_SWI = 1;
static const int ARM_CALLBACK_IRQ = 2;
static const int ARM_CALLBACK_SWI_RET = 3;
static const int ARM_CALLBACK_IRQ_RET = 4;
static const int ARM_CALLBACK_CALL = 5;
static const int ARM_CALLBACK_RET = 6;

typedef struct 
{
    u32 address;
    bool thumb;
} ARMCallbackExecute;

typedef void (*ARMCallback)(int reason, void* data);

using namespace std;

namespace NanoboyAdvance
{
    class ARM7
    {
        // Grants the processor access to the emulated mmu
        GBAMemory* memory;

        // The ARM7TMDI-S has 31 32-bit general purpose register of
        // which 16 are visible at one time.
        u32 r0 {0}, r1 {0}, r2 {0}, r3 {0}, r4 {0}, r5 {0}, r6 {0}, r7 {0}, r8 {0}, r9 {0}, r10 {0}, r11 {0}, r12 {0}, r13 {0}, r14 {0}, r15 {0};
        u32 r8_fiq {0}, r9_fiq {0}, r10_fiq {0}, r11_fiq {0}, r12_fiq {0}, r13_fiq {0}, r14_fiq {0};
        u32 r13_svc {0}, r14_svc {0};
        u32 r13_abt {0}, r14_abt {0};
        u32 r13_irq {0}, r14_irq {0};
        u32 r13_und {0}, r14_und {0};

        // Mapping array for visible general purpose registers
        u32* gprs[16];

        // Current program status register (contains status flags)
        u32 cpsr { (u32)ARM7Mode::System};
        u32 spsr_fiq {0}, spsr_svc {0}, spsr_abt {0}, spsr_irq {0}, spsr_und {0}, spsr_def {0};

        // A pointer pointing on the Saved program status register of the current mode
        u32* pspsr {nullptr};

        #ifdef ARM7_FASTHAX
        int thumb_decode[0x10000];
        int arm_decode[0x100000];
        #endif

        // In some way emulate the processor's pipeline
        u32 pipe_opcode[3];
        int pipe_decode[3];
        int pipe_status {0};
        bool flush_pipe {false};

        // Emulate "unpredictable" behaviour
        u32 last_fetched_opcode {0};
        u32 last_fetched_offset {0};
        u32 last_bios_offset {0};

        // Gets called on certain events like instruction execution, swi, etc.
        // Do not ever call this directly! Use safe DebugHook instead!
        ARMCallback debug_hook { NULL };
        
        // Indicates wether interrupts and swi should be processed using
        // the bios or using a hle attempt
        bool hle;

        // Maps the visible registers (according to cpsr) to gprs
        inline void RemapRegisters()
        {
            switch (cpsr & 0x1F)
            {
            case (u32)ARM7Mode::User:
                gprs[8] = &r8;
                gprs[9] = &r9;
                gprs[10] = &r10;
                gprs[11] = &r11;
                gprs[12] = &r12;
                gprs[13] = &r13;
                gprs[14] = &r14;
                pspsr = &spsr_def;
                break;
            case (u32)ARM7Mode::FIQ:
                gprs[8] = &r8_fiq;
                gprs[9] = &r9_fiq;
                gprs[10] = &r10_fiq;
                gprs[11] = &r11_fiq;
                gprs[12] = &r12_fiq;
                gprs[13] = &r13_fiq;
                gprs[14] = &r14_fiq;
                pspsr = &spsr_fiq;
                break;
            case (u32)ARM7Mode::IRQ:
                gprs[8] = &r8;
                gprs[9] = &r9;
                gprs[10] = &r10;
                gprs[11] = &r11;
                gprs[12] = &r12;
                gprs[13] = &r13_irq;
                gprs[14] = &r14_irq;
                pspsr = &spsr_irq;
                break;
            case (u32)ARM7Mode::SVC:
                gprs[8] = &r8;
                gprs[9] = &r9;
                gprs[10] = &r10;
                gprs[11] = &r11;
                gprs[12] = &r12;
                gprs[13] = &r13_svc;
                gprs[14] = &r14_svc;
                pspsr = &spsr_svc;
                break;
            case (u32)ARM7Mode::Abort:
                gprs[8] = &r8;
                gprs[9] = &r9;
                gprs[10] = &r10;
                gprs[11] = &r11;
                gprs[12] = &r12;
                gprs[13] = &r13_abt;
                gprs[14] = &r14_abt;
                pspsr = &spsr_abt;
                break;
            case (u32)ARM7Mode::Undefined:
                gprs[8] = &r8;
                gprs[9] = &r9;
                gprs[10] = &r10;
                gprs[11] = &r11;
                gprs[12] = &r12;
                gprs[13] = &r13_und;
                gprs[14] = &r14_und;
                pspsr = &spsr_und;
                break;
            case (u32)ARM7Mode::System:
                gprs[8] = &r8;
                gprs[9] = &r9;
                gprs[10] = &r10;
                gprs[11] = &r11;
                gprs[12] = &r12;
                gprs[13] = &r13;
                gprs[14] = &r14;
                pspsr = &spsr_def;
                break;
            }
        }
        
        // Pointer-safe call to debug_hook (avoid nullpointer)
        inline void DebugHook(int reason, void* data)
        {
            if (debug_hook != NULL)
            {
                debug_hook(reason, data);
            }
        }

        // Condition code altering methods
        inline void CalculateSign(u32 result)
        {
            cpsr = result & 0x80000000 ? (cpsr | SignFlag) : (cpsr & ~SignFlag);
        }

        inline void CalculateZero(u64 result)
        {
            cpsr = result == 0 ? (cpsr | ZeroFlag) : (cpsr & ~ZeroFlag);
        }

        inline void AssertCarry(bool carry)
        {
            cpsr = carry ? (cpsr | CarryFlag) : (cpsr & ~CarryFlag);
        }

        inline void CalculateOverflowAdd(u32 result, u32 operand1, u32 operand2)
        {
            bool overflow = ((operand1) >> 31 == (operand2) >> 31) && ((result) >> 31 != (operand2) >> 31);
            cpsr = overflow ? (cpsr | OverflowFlag) : (cpsr & ~OverflowFlag);
        }

        inline void CalculateOverflowSub(u32 result, u32 operand1, u32 operand2)
        {
            bool overflow = ((operand1) >> 31 != (operand2) >> 31) && ((result) >> 31 == (operand2) >> 31);
            cpsr = overflow ? (cpsr | OverflowFlag) : (cpsr & ~OverflowFlag);
        }

        // Shifter methods
        inline void LSL(u32& operand, u32 amount, bool& carry)
        {
            // Nothing is done when the shift amount equals zero
            if (amount != 0)
            {
                #ifndef __i386__
                #ifdef __arm__
                asm volatile("movs %0, %2, lsl %3\n"
                                        "movcs %1, #1\n"
                                        "movcc %1, #0" : "=r" (operand), "=r" (carry) : "r" (operand), "r" (amount) : "cc"); 
                #else
                carry = (operand << (amount - 1)) & 0x80000000 ? true : false;
                operand <<= amount;
                #endif
                #else 
                // This way we easily bypass the 32 bits restriction on x86
                for (u32 i = 0; i < amount; i++)
                {
                    carry = operand & 0x80000000 ? true : false;
                    operand <<= 1;
                }
                #endif
            }
        }

        inline void LSR(u32& operand, u32 amount, bool& carry, bool immediate)
        {
            // LSR #0 equals to LSR #32
            amount = immediate & (amount == 0) ? 32 : amount;

            // Perform shift
            #ifndef __i386__
            #ifdef __arm__
            asm volatile("movs %0, %2, lsr %3\n"
                                    "movcs %1, #1\n"
                                    "movcc %1, #0" : "=r" (operand), "=r" (carry) : "r" (operand), "r" (amount) : "cc"); 
            #else
            carry = (operand >> (amount - 1)) & 1;
            operand >>= amount;
            #endif
            #else
            for (u32 i = 0; i < amount; i++)
            {
                carry = operand & 1 ? true : false;
                operand >>= 1;
            }
            #endif
        }

        inline void ASR(u32& operand, u32 amount, bool& carry, bool immediate)
        {
            u32 sign_bit = operand & 0x80000000;

            // ASR #0 equals to ASR #32
            amount = immediate & (amount == 0) ? 32 : amount;

            // Perform shift
            #ifdef __arm__
            asm volatile("movs %0, %2, asr %3\n"
                                    "movcs %1, #1\n"
                                    "movcc %1, #0" : "=r" (operand), "=r" (carry) : "r" (operand), "r" (amount) : "cc"); 
            #else
            for (u32 i = 0; i < amount; i++)
            {
                carry = operand & 1 ? true : false;
                operand = (operand >> 1) | sign_bit;
            }
            #endif
        }

        inline void ROR(u32& operand, u32 amount, bool& carry, bool immediate)
        {
            // ROR #0 equals to RRX #1
            if (amount != 0 || !immediate)
            {
                //#ifdef __arm__
                //// todo: fix arm version
                //asm volatile("movs %0, %2, ror %3\n"
                //                        "movcs %1, #1\n"
                //                        "movcc %1, #0" : "=r" (operand), "=r" (carry) : "r" (operand), "r" (amount) : "cc"); 
                //#else
                for (u32 i = 1; i <= amount; i++)
                {
                    u32 high_bit = (operand & 1) ? 0x80000000 : 0;
                    operand = (operand >> 1) | high_bit;
                    carry = high_bit == 0x80000000;
                }
                //#endif
            }
            else
            {
                bool old_carry = carry; // todo: optimize with inline asm
                carry = (operand & 1) ? true : false;
                operand = (operand >> 1) | (old_carry ? 0x80000000 : 0);
            }
        }

        // Memory methods
        inline u8 ReadByte(u32 offset)
        {
            return memory->ReadByte(offset);
        }

        inline u16 ReadHWord(u32 offset)
        {
            if (offset & 1)
            {
                u16 value = memory->ReadHWord(offset & ~1);
                return (value >> 8) | (value << 24); 
            }        
            return memory->ReadHWord(offset);
        }

        inline u32 ReadHWordSigned(u32 offset)
        {
            u32 value = 0;        
            if (offset & 1) 
            {
                value = memory->ReadByte(offset & ~1);
                if (value & 0x80) value |= 0xFFFFFF00;
            }
            else 
            {
                value = memory->ReadHWord(offset);
                if (value & 0x8000) value |= 0xFFFF0000;
            }
            return value;
        }

        inline u32 ReadWord(u32 offset)
        {
            if (offset < 0x4000 && last_fetched_offset >= 0x4000)
            {
                return memory->ReadWord(last_bios_offset);
            }
            if (offset >= 0x4000 && offset < 0x2000000)
            {
                if (cpsr & Thumb)
                {
                    // TODO: Handle special cases
                    return (last_fetched_opcode << 16) | last_fetched_opcode;
                }
                else
                {
                    return last_fetched_opcode;
                }
            }
            return memory->ReadWord(offset & ~3);
        }

        inline u32 ReadWordRotated(u32 offset)
        {
            u32 value = ReadWord(offset & ~3);
            int amount = (offset & 3) * 8;
            return amount == 0 ? value : ((value >> amount) | (value << (32 - amount)));
        }

        inline void WriteByte(u32 offset, u8 value)
        {
            memory->WriteByte(offset, value);
        }

        inline void WriteHWord(u32 offset, u16 value)
        {
            offset &= ~1;
            memory->WriteHWord(offset, value);
        }

        inline void WriteWord(u32 offset, u32 value)
        {
            offset &= ~3;
            memory->WriteWord(offset, value);
        }

        // Command processing
        int ARMDecode(u32 instruction);
        void ARMExecute(u32 instruction, int type);
        int THUMBDecode(u16 instruction);
        void THUMBExecute(u16 instruction, int type);

        // Used to emulate software interrupts
        void SWI(int number);
    public:
        enum class ARM7Mode
        {
            User = 0x10,
            FIQ = 0x11,
            IRQ = 0x12,
            SVC = 0x13,
            Abort = 0x17,
            Undefined = 0x1B,
            System = 0x1F
        };
        enum CPSRFlags // TODO: enum class
        {
            Thumb = 0x20,
            FIQDisable = 0x40,
            IRQDisable = 0x80,
            OverflowFlag = 0x10000000,
            CarryFlag = 0x20000000,
            ZeroFlag = 0x40000000,
            SignFlag = 0x80000000
        };

        // Constructor
        ARM7(GBAMemory* memory, bool use_bios);
        
        // Execution functions
        void Step(); // schedule pipeline
        void FireIRQ(); // enter bios irq handler

        // Debugging
        u32 GetGeneralRegister(ARM7Mode mode, int r);
        u32 GetCurrentStatusRegister();
        u32 GetSavedStatusRegister(ARM7Mode mode);  
        void SetCallback(ARMCallback hook);
        void SetGeneralRegister(ARM7Mode mode, int r, u32 value);
        void SetCurrentStatusRegister(u32 value);
        void SetSavedStatusRegister(ARM7Mode mode, u32 value);     
    };
}
