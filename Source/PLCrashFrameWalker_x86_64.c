/*
 * Author: Gwynne Raskind <gwynne@darkrainfall.org>
 * Author: Landon Fuller <landonf@plausiblelabs.com>
 *
 * Copyright (c) 2008-2012 Plausible Labs Cooperative, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#import "PLCrashFrameWalker.h"
#import "PLCrashAsync.h"
#import "PLCrashAsyncImage.h"

#import <signal.h>
#import <assert.h>
#import <stdlib.h>

#ifdef __x86_64__

// PLFrameWalker API
plframe_error_t plframe_cursor_init (plframe_cursor_t *cursor, ucontext_t *uap, plcrash_async_image_list_t *image_list) {
    int result = 0;

    cursor->uap = uap;
    cursor->nframe = -1;
    cursor->fp[0] = NULL;
    cursor->image_list = image_list;
    cursor->endstack = false;
    cursor->last_unwind_address = 0;
    cursor->last_stack_pointer = cursor->uap->uc_mcontext->__ss.__rsp;
    
    /* The first valid frame is the current instruction, by definition. */
    cursor->last_valid_frame = cursor->uap->uc_mcontext->__ss.__rip;
    
    /*
        libunwind's internal structures are undocumented and unreliable, but
        there's currently no supported way to set up an arbitrary context other
        than being on the thread in question. See:
        http://opensource.apple.com/source/libunwind/libunwind-30/src/Registers.hpp
        for "documentation" on why this "works".
    */
    result = unw_init_local(&cursor->unwcrsr, (unw_context_t *)&cursor->uap->uc_mcontext->__ss);

    /* libunwind never returns an error from unw_init_local(3); this check is
       for the sake of correctness.
    */
    return plframe_error_from_unwerror(result);
}

// PLFrameWalker API
plframe_error_t plframe_cursor_thread_init (plframe_cursor_t *cursor, thread_t thread, plcrash_async_image_list_t *image_list) {
    kern_return_t kr;
    ucontext_t *uap;
    
    /*
        Note: This code has been left untouched when implementing libunwind(3)
        usage, as 1) Apple's implementation of libunwind on x86_64 doesn't
        handle floating-point and vector registers, 2) libunwind's general API
        doesn't provide access to some of the other information retrieved here.
    */
    
    /* Perform basic initialization */
    uap = &cursor->_uap_data;
    uap->uc_mcontext = (void *) &cursor->_mcontext_data;
    
    /* Zero the signal mask */
    sigemptyset(&uap->uc_sigmask);
    
    /* Fetch the thread states */
    mach_msg_type_number_t state_count;
    
    /* Sanity check */
    assert(sizeof(cursor->_mcontext_data.__ss) == sizeof(x86_thread_state64_t));
    assert(sizeof(cursor->_mcontext_data.__es) == sizeof(x86_exception_state64_t));
    assert(sizeof(cursor->_mcontext_data.__fs) == sizeof(x86_float_state64_t));
    
    // thread state
    state_count = x86_THREAD_STATE64_COUNT;
    kr = thread_get_state(thread, x86_THREAD_STATE64, (thread_state_t) &cursor->_mcontext_data.__ss, &state_count);
    if (kr != KERN_SUCCESS) {
        PLCF_DEBUG("Fetch of x86-64 thread state failed with mach error: %d", kr);
        return PLFRAME_INTERNAL;
    }
    
    // floating point state
    state_count = x86_FLOAT_STATE64_COUNT;
    kr = thread_get_state(thread, x86_FLOAT_STATE64, (thread_state_t) &cursor->_mcontext_data.__fs, &state_count);
    if (kr != KERN_SUCCESS) {
        PLCF_DEBUG("Fetch of x86-64 float state failed with mach error: %d", kr);
        return PLFRAME_INTERNAL;
    }
    
    // exception state
    state_count = x86_EXCEPTION_STATE64_COUNT;
    kr = thread_get_state(thread, x86_EXCEPTION_STATE64, (thread_state_t) &cursor->_mcontext_data.__es, &state_count);
    if (kr != KERN_SUCCESS) {
        PLCF_DEBUG("Fetch of x86-64 exception state failed with mach error: %d", kr);
        return PLFRAME_INTERNAL;
    }
    
    /* Perform standard initialization and return result */
    return plframe_cursor_init(cursor, uap, image_list);
}


static bool plframe_cursor_address_looks_valid (plframe_cursor_t *cursor, uintptr_t address)
{
    /* Optimization: The entire bottom 4GB of address space is known to be
       invalid on OS X. Immediately return false if the address is in that
       range. */
    if ((address & 0xFFFFFFFF00000000) == 0)
        return false;
    
    plcrash_async_image_t *entry = NULL;
    
    /* Loop over all loaded images, checking whether the address is within its
       VM range. Facilities for checking whether the address falls within a
       valid symbol are unsafe at async-signal time. */
    //PLCF_DEBUG("cursor_address_looks_valid(): trying address %llx", (uint64_t)address);
    plcrash_async_image_list_set_reading(cursor->image_list, true);
    while ((entry = plcrash_async_image_list_next(cursor->image_list, entry)) != NULL) {
        if (address >= entry->image.header && address <= entry->image.header + entry->image.textsize) {
            //PLCF_DEBUG("cursor_address_looks_valid(): found address %llx in image with range %llx to %llx", (uint64_t)address,
            //          (uint64_t)entry->image.header, (uint64_t)entry->image.header + entry->image.textsize);
            plcrash_async_image_list_set_reading(cursor->image_list, false);
            return true;
        }
    }
    plcrash_async_image_list_set_reading(cursor->image_list, false);
    return false;
}

plframe_error_t plframe_cursor_scan_stack (plframe_cursor_t *cursor)
{
    /* This is the number of words of stack the scanner will look through before
       giving up. */
    const size_t search_space = 500;
    
    for (plframe_greg_t loc = cursor->last_stack_pointer;
         loc <= cursor->last_stack_pointer + (search_space * sizeof(plframe_greg_t));
         loc += sizeof(plframe_greg_t))
    {
        plframe_greg_t data;
        kern_return_t result;
        
        result = plframe_read_addr((const void *)loc, &data, sizeof(data));
        if (result != KERN_SUCCESS) {
            /* ran off the end of the stack; treat it as no more frames */
            //PLCF_DEBUG("cursor_scan_stack(): ran off the end of the stack, returning end of frames");
            return PLFRAME_ENOFRAME;
        }
        if (plframe_cursor_address_looks_valid(cursor, data)) {
            /* This is a valid address in some loaded address space. Cross
               fingers and hope, because that's all the checks we can do at
               async signal time. Record the address, advance the saved stack
               pointer, and return success. */
            //PLCF_DEBUG("cursor_scan_stack(): found apparently valid address %llx, running with it", (uint64_t)data);
            cursor->last_stack_pointer = loc + sizeof(plframe_greg_t);
            cursor->last_valid_frame = data;
            return PLFRAME_ESUCCESS;
        }
    }
    //PLCF_DEBUG("cursor_scan_stack(): found no valid addresses within %ld words, giving up", search_space);
    return PLFRAME_ENOFRAME;
}

// PLFrameWalker API
plframe_error_t plframe_cursor_next (plframe_cursor_t *cursor) {
    /* libunwind will always give a correct result for the top frame on the
       stack, so for the first frame, don't bother cross-checking for a scan. */
    if (cursor->nframe == -1) {
        /* The first frame is loaded by unw_init_local(), so no action needed */
        ++cursor->nframe;
        //PLCF_DEBUG("cursor_next(): first frame; returning success");
    } else if (cursor->endstack) {
        return PLFRAME_ENOFRAME;
    } else {
        int unwr;
        
        /* libunwind hasn't signaled a stack end yet; record the last address
           retrieved from it as valid */
        unw_get_reg(&cursor->unwcrsr, UNW_REG_IP, &cursor->last_unwind_address);
        //PLCF_DEBUG("cursor_next(): libunwind context is still valid; record frame %llx and step via libunwind", (uint64_t)cursor->last_unwind_address);
        unwr = unw_step(&cursor->unwcrsr);
        ++cursor->nframe;
        if (unwr < 0) {
            //PLCF_DEBUG("cursor_next(): libunwind error %d", unwr);
            return plframe_error_from_unwerror(unwr);
        } else {
            unw_word_t reg;
            
            unw_get_reg(&cursor->unwcrsr, UNW_REG_IP, &reg);
            if (unwr == 0) {
                /* If libunwind returned an "end stack", one of two things has
                   happened:
                   1) It really is the bottom of the stack.
                   2) libunwind didn't find any unwinding info for the current
                      frame and decided to stop trying.
                   The difference between these cases is detected by checking
                   for a duplicated frame in the unwind cursor - if libunwind
                   returns the same frame twice in a row and signals end at the
                   same time, it failed to find any unwinding information. There
                   is no better way to detect this case without digging into
                   libunwind internals.
                */
                //PLCF_DEBUG("cursor_next(): libunwind signaled end; checking new IP %llx vs last unwind IP %llx", (uint64_t)reg, (uint64_t)cursor->last_unwind_address);
                if (cursor->last_unwind_address == reg) {
                    plcrash_error_t result;
                    /* libunwind returned a duplicate on stack end; try a stack
                       scan, which updates last_valid_frame. */
                    //PLCF_DEBUG("cursor_next(): duplicate address detected, trying stack scan and resetting libunwind");
                    result = plframe_cursor_scan_stack(cursor);
                    if (result == PLFRAME_ENOFRAME) {
                        //PLCF_DEBUG("cursor_next(): stack scan found the end of the stack");
                        cursor->endstack = true;
                    } else if (result != PLFRAME_ESUCCESS) {
                        //PLCF_DEBUG("cursor_next(): stack scan error %d", result);
                        return result;
                    } else {
                        x86_thread_state64_t state;
                        
                        /* Reset libunwind using the results from the stack scan
                           by shoehorning the valid address into a new context
                           pointer. Again, this is undocumented, unsupported,
                           and ugly. */
                        //PLCF_DEBUG("cursor_next(): stack scan found a valid frame with IP %llx", (uint64_t)cursor->last_valid_frame);
                        state = cursor->uap->uc_mcontext->__ss;
                        state.__rip = cursor->last_valid_frame;
                        return plframe_error_from_unwerror(unw_init_local(&cursor->unwcrsr, (unw_context_t *)&state));
                    }
                } else {
                    //PLCF_DEBUG("cursor_next(): libunwind saw legitimate stack end, ending unwind");
                    /* libunwind did not return a duplicate; this really is the
                       end of the stack */
                    cursor->endstack = true;
                }
            }
            unw_get_reg(&cursor->unwcrsr, UNW_REG_IP, &cursor->last_valid_frame);
            //PLCF_DEBUG("cursor_next(): libunwind stepped successfully, recording current address %llx as valid frame", (uint64_t)cursor->last_valid_frame);
        }
    }
    return PLFRAME_ESUCCESS;
}


// PLFrameWalker API
#define RETGEN(name, type, uap, result) {\
    *result = (uap->uc_mcontext->__ ## type . __ ## name); \
    return PLFRAME_ESUCCESS; \
}

plframe_error_t plframe_get_reg (plframe_cursor_t *cursor, plframe_regnum_t regnum, plframe_greg_t *reg) {
    ucontext_t *uap = cursor->uap;
    unw_regnum_t unwreg;
    unw_word_t regval;
    int result;
    
    if (cursor->nframe != 0) {
        if (regnum == PLFRAME_X86_64_RIP) {
            *reg = cursor->last_valid_frame;
            return PLFRAME_ESUCCESS;
        }
        return PLFRAME_ENOTSUP;
    }
    
    #define MAP_REG(reg)		\
        case PLFRAME_X86_64_ ## reg:	\
            unwreg = UNW_X86_64_ ## reg;	\
            break
    
    switch (regnum) {
        MAP_REG(RAX);
        MAP_REG(RBX);
        MAP_REG(RCX);
        MAP_REG(RDX);
        MAP_REG(RDI);
        MAP_REG(RSI);
        MAP_REG(RBP);
        MAP_REG(RSP);
        MAP_REG(R10);
        MAP_REG(R11);
        MAP_REG(R12);
        MAP_REG(R13);
        MAP_REG(R14);
        MAP_REG(R15);
        
        /* Manual mappings */
        case PLFRAME_X86_64_RIP:
            unwreg = UNW_REG_IP;
            break;
        
        /* These registers are not available through the libunwind API. */
        case PLFRAME_X86_64_RFLAGS:
            RETGEN(rflags, ss, uap, reg);
            
        case PLFRAME_X86_64_CS:
            RETGEN(cs, ss, uap, reg);
            
        case PLFRAME_X86_64_FS:
            RETGEN(fs, ss, uap, reg);
            
        case PLFRAME_X86_64_GS:
            RETGEN(gs, ss, uap, reg);
        
        default:
            return PLFRAME_ENOTSUP;
    }
    
    #undef MAP_REG
    
    result = unw_get_reg(&cursor->unwcrsr, unwreg, &regval);
    if (result == UNW_ESUCCESS) {
        *reg = regval;
        return PLFRAME_ESUCCESS;
    } else {
        return plframe_error_from_unwerror(result);
    }
    
    return PLFRAME_ENOTSUP;
}

#undef RETGEN


// PLFrameWalker API
plframe_error_t plframe_get_freg (plframe_cursor_t *cursor, plframe_regnum_t regnum, plframe_fpreg_t *fpreg) {
    return PLFRAME_ENOTSUP;
}

// PLFrameWalker API
const char *plframe_get_regname (plframe_regnum_t regnum) {
    switch (regnum) {
        case PLFRAME_X86_64_RAX:
            return "rax";

        case PLFRAME_X86_64_RBX:
            return "rbx";
            
        case PLFRAME_X86_64_RCX:
            return "rcx";
            
        case PLFRAME_X86_64_RDX:
            return "rdx";
            
        case PLFRAME_X86_64_RDI:
            return "rdi";
            
        case PLFRAME_X86_64_RSI:
            return "rsi";
            
        case PLFRAME_X86_64_RBP:
            return "rbp";
            
        case PLFRAME_X86_64_RSP:
            return "rsp";
            
        case PLFRAME_X86_64_R10:
            return "r10";
            
        case PLFRAME_X86_64_R11:
            return "r11";
            
        case PLFRAME_X86_64_R12:
            return "r12";
            
        case PLFRAME_X86_64_R13:
            return "r13";
            
        case PLFRAME_X86_64_R14:    
            return "r14";
            
        case PLFRAME_X86_64_R15:
            return "r15";
            
        case PLFRAME_X86_64_RIP:
            return "rip";
            
        case PLFRAME_X86_64_RFLAGS:
            return "rflags";
            
        case PLFRAME_X86_64_CS:
            return "cs";
            
        case PLFRAME_X86_64_FS:
            return "fs";
            
        case PLFRAME_X86_64_GS:
            return "gs";
            
        default:
            // Unsupported register
            break;
    }
    
    /* Unsupported register is an implementation error (checked in unit tests) */
    PLCF_DEBUG("Missing register name for register id: %d", regnum);
    abort();
}

#endif
