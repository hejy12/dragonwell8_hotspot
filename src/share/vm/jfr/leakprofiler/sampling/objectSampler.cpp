/*
 * Copyright (c) 2014, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */
#include "precompiled.hpp"
#include "jfr/recorder/jfrEventSetting.hpp"
#include "jfr/recorder/access/jfrThreadData.hpp"
#include "jfr/recorder/checkpoint/jfrCheckpointManager.hpp"
#include "jfr/recorder/stacktrace/jfrStackTraceRepository.hpp"
#include "jfr/leakprofiler/sampling/objectSample.hpp"
#include "jfr/leakprofiler/sampling/objectSampler.hpp"
#include "jfr/leakprofiler/sampling/sampleList.hpp"
#include "jfr/leakprofiler/sampling/samplePriorityQueue.hpp"
#include "jfr/utilities/jfrTryLock.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/thread.hpp"
#include "trace/tracing.hpp"

ObjectSampler::ObjectSampler(size_t size) :
  _priority_queue(new SamplePriorityQueue(size)),
  _list(new SampleList(size)),
  _last_sweep(Tracing::time()),
  _total_allocated(0),
  _threshold(0),
  _size(size),
  _tryLock(0),
  _dead_samples(false) {}

ObjectSampler::~ObjectSampler() {
  delete _priority_queue;
  _priority_queue = NULL;
  delete _list;
  _list = NULL;
}

void ObjectSampler::add(HeapWord* obj, size_t allocated, JavaThread* thread) {
  assert(thread != NULL, "invariant");
  const traceid thread_id = thread->threadObj() != NULL ? thread->trace_data()->thread_id() : 0;
  if (thread_id == 0) {
    return;
  }
  assert(thread_id != 0, "invariant");

  if (!thread->trace_data()->has_thread_checkpoint()) {
    JfrCheckpointManager::create_thread_checkpoint(thread);
    assert(thread->trace_data()->has_thread_checkpoint(), "invariant");
  }

  traceid stack_trace_id = 0;
  unsigned int stack_trace_hash = 0;
  if (JfrEventSetting::has_stacktrace(EventOldObjectSample::eventId)) {
    stack_trace_id = JfrStackTraceRepository::record(thread, 0, &stack_trace_hash);
    thread->trace_data()->set_cached_stack_trace_id(stack_trace_id, stack_trace_hash);
  }

  const JfrTraceTime allocation_time = Tracing::time();

  JfrTryLock tryLock(&_tryLock);
  if (!tryLock.has_lock()) {
    log_trace(jfr, oldobject, sampling)("Skipping old object sample due to lock contention");
    return;
  }

  if (_dead_samples) {
    scavenge();
    assert(!_dead_samples, "invariant");
  }

  _total_allocated += allocated;
  const size_t span = _total_allocated - _priority_queue->total();
  ObjectSample* sample;
  if ((size_t)_priority_queue->count() == _size) {
    assert(_list->count() == _size, "invariant");
    const ObjectSample* peek = _priority_queue->peek();
    if (peek->span() > span) {
      // quick reject, will not fit
      return;
    }
    sample = _list->reuse(_priority_queue->pop());
  } else {
    sample = _list->get();
  }

  assert(sample != NULL, "invariant");
  assert(thread_id != 0, "invariant");
  sample->set_thread_id(thread_id);
  sample->set_thread_checkpoint(thread->trace_data()->thread_checkpoint());

  if (stack_trace_id != 0) {
    sample->set_stack_trace_id(stack_trace_id);
    sample->set_stack_trace_hash(stack_trace_hash);
  }

  sample->set_span(allocated);
  sample->set_object((oop)obj);
  sample->set_allocated(allocated);
  sample->set_allocation_time(allocation_time);
  _priority_queue->push(sample);
}

const ObjectSample* ObjectSampler::last() const {
  return _list->last();
}

const ObjectSample* ObjectSampler::last_resolved() const {
  return _list->last_resolved();
}

void ObjectSampler::set_last_resolved(const ObjectSample* sample) {
  _list->set_last_resolved(sample);
}

void ObjectSampler::oops_do(BoolObjectClosure* is_alive, OopClosure* f) {
  ObjectSample* current = _list->last();
  while (current != NULL) {
    ObjectSample* next = current->next();
    if (!current->is_dead()) {
      if (is_alive->do_object_b(current->object())) {
        // The weakly referenced object is alive, update pointer
        f->do_oop(const_cast<oop*>(current->object_addr()));
      } else {
        current->set_dead();
        _dead_samples = true;
      }
    }
    current = next;
  }
  _last_sweep = Tracing::time();
}

void ObjectSampler::remove_dead(ObjectSample* sample) {
  assert(sample != NULL, "invariant");
  assert(sample->is_dead(), "invariant");
  ObjectSample* const previous = sample->prev();
  // push span on to previous
  if (previous != NULL) {
    _priority_queue->remove(previous);
    previous->add_span(sample->span());
    _priority_queue->push(previous);
  }
  _priority_queue->remove(sample);
  _list->release(sample);
}

void ObjectSampler::scavenge() {
  ObjectSample* current = _list->last();
  while (current != NULL) {
    ObjectSample* next = current->next();
    if (current->is_dead()) {
      remove_dead(current);
    }
    current = next;
  }
  _dead_samples = false;
}

int ObjectSampler::item_count() const {
  return _priority_queue->count();
}

const ObjectSample* ObjectSampler::item_at(int index) const {
  return _priority_queue->item_at(index);
}

ObjectSample* ObjectSampler::item_at(int index) {
  return const_cast<ObjectSample*>(
    const_cast<const ObjectSampler*>(this)->item_at(index)
                                   );
}

const JfrTraceTime& ObjectSampler::last_sweep() const {
  return _last_sweep;
}
