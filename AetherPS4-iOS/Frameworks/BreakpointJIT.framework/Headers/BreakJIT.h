//
//  BreakJIT.h
//  BreakpointJIT
//
//  Created by Stossy11 on 09/07/2025.
//

#ifndef BreakGetJITMapping_h
#define BreakGetJITMapping_h

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Function with inline assembly for JIT mapping
 * @param bytes Size parameter for mapping
 * @return char* pointer result
 */
__attribute__((noinline, optnone, naked)) void* BreakGetJITMapping(void *addr, size_t len);

__attribute__((noinline,optnone,naked)) void BreakJITDetach(void);

__attribute__((noinline, optnone, naked)) void* BreakMarkJITMapping(size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* BreakGetJITMapping_h */
