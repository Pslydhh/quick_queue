// Copyright 2021 Pslydhh. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <linux/futex.h>
#include <pthread.h>
#include <syscall.h>
#include <unistd.h>
#include <limits.h>

#include <cassert>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <stack>
#include <vector>

#include "align.h"
#include "primitives.h"

template <typename T>
class ScalableBoundedBlockingQueue {
    static_assert(sizeof(uintptr_t) <= sizeof(void*),
                  "void* pointer can hold every data pointer, So Its size at least as uintptr_t");

public:
    static constexpr int NODE_SIZE = 10000;

    struct Cell {
        Cell() : data_field(), control_field(0), put_version_field(0), pop_version_field(0) {}
        T data_field;

        int64_t control_field;
        int64_t put_version_field;
        int64_t pop_version_field;
    };

    struct node_t {
        node_t() : next(nullptr), id(0), cells() {}

        node_t* next DOUBLE_CACHE_ALIGNED;
        long id DOUBLE_CACHE_ALIGNED;
        Cell cells[NODE_SIZE] DOUBLE_CACHE_ALIGNED;
    };

    static inline node_t* ob_new_node() {
        //node_t* n = reinterpret_cast<node_t*>(align_malloc(PAGE_SIZE, sizeof(node_t)));

        node_t* n = new node_t();
        //memset(n, 0, sizeof(node_t));

        return n;
    }

    ScalableBoundedBlockingQueue(int threshold = 8)
            : init_node(ob_new_node()),
              init_id(0),
              put_index(0),
              pop_index(0),
              threshold(threshold),
              mutex() {}

    ~ScalableBoundedBlockingQueue() {
        do {
            node_t* node = init_node;
            init_node = node->next;
            delete node;
        } while (init_node != nullptr);
    }

    node_t* init_node;
    long init_id DOUBLE_CACHE_ALIGNED;

    int64_t put_index DOUBLE_CACHE_ALIGNED;
    int64_t pop_index DOUBLE_CACHE_ALIGNED;

    static constexpr int HANDLES = 1024;

    int threshold;

    std::mutex mutex;

    /*
    * ob_find_cell: This is our core operation, locating the offset on the nodes and nodes needed.
    */
    static Cell* ob_find_cell(node_t** ptr, long i) {
        // get current node
        node_t* curr = LOAD(ptr);
        return &curr->cells[i % NODE_SIZE];
    }

    static int ob_futex_wake(void* addr, uint32_t val) {
        return syscall(SYS_futex, addr, FUTEX_WAKE, val, NULL, NULL, 0);
    }

    static int ob_futex_wait(void* addr, uint32_t val) {
        return syscall(SYS_futex, addr, FUTEX_WAIT, val, NULL, NULL, 0);
    }

    void put(T v) {
        // Correctness guarantee: Mutual-Confirming
        int64_t put_index_local = FAA(&put_index, 1);
        Cell* c = ob_find_cell(&init_node, put_index_local);

        int64_t put_version_field_local;
        while ((put_version_field_local = LOAD(&c->put_version_field)) <
               (put_index_local / NODE_SIZE)) {
            ob_futex_wait(&c->put_version_field, put_version_field_local);
        }
        LOADa(&c->put_version_field);

        c->data_field = v;

        int64_t local;
        if ((local = XCHG(&c->control_field, 2)) != 0) {
            ob_futex_wake(&c->control_field, 1);
            assert(local == 1);
        }
    }

    T blocking_get() {
        // Correctness guarantee: Mutual-Confirming
        int64_t pop_index_local = FAA(&pop_index, 1);
        Cell* c = ob_find_cell(&init_node, pop_index_local);

        int64_t pop_version_field_local;
        while ((pop_version_field_local = LOAD(&c->pop_version_field)) <
               (pop_index_local / NODE_SIZE)) {
            ob_futex_wait(&c->pop_version_field, pop_version_field_local);
        }
        LOADa(&c->pop_version_field);

        int times = (1 << 5);
        do {
            int cv = LOAD(&c->control_field);
            if (cv) {
                LOADa(&c->control_field);
                goto over;
            }
            PAUSE();
        } while (times-- > 0);

        int64_t local;
        if ((local = XCHG(&c->control_field, 1)) == 0) {
            do {
                ob_futex_wait(&c->control_field, 1);
            } while (LOAD(&c->control_field) == 1);
            LOADa(&c->control_field);
        } else {
            assert(local == 2);
        }

    over:
        STORE(&c->control_field, 0);
        T local_result = c->data_field;

        // Correctness guarantee: Mutual-Confirming
        local = LOAD(&c->put_version_field);
        STOREr(&c->put_version_field, local + 1);
        local = LOAD(&c->pop_version_field);
        STOREr(&c->pop_version_field, local + 1);

        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        if ((LOAD(&put_index) - pop_index_local) > NODE_SIZE) {
            ob_futex_wake(&c->put_version_field, INT_MAX);
        }
        if ((LOAD(&pop_index) - pop_index_local) > NODE_SIZE) {
            ob_futex_wake(&c->pop_version_field, INT_MAX);
        }

        return local_result;
    }
};
