#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "../root_iterator.h"
#include "../root.h"
#include "../heap.h"
#include "../config.h"
#include "../region.h"
#include "../descriptor.h"
#include "../profiler.h"

#include "young_collector.h"
#include "gc.h"
#include "marker.h"

bool gc_full_pre_collect(struct gc_state* self) {
  for (int i = 0; i < self->heap->oldGeneration->sizeInCells; i++)
    self->fullGC.oldLookup[i] = NULL;
  
  return true;
}

static void fixRefs(struct gc_state* self) {
  struct heap* heap = self->heap;  
  
  gc_fixer_callback fixer = ^void* (void* ptr) {
    if (!ptr)
      return NULL;

    // Unrecognized leave as it is
    struct region* region = heap_get_region(heap, ptr);
    if (region != heap->oldGeneration)
      return ptr;

    int id = region_get_cellid(region, ptr);
    struct region_reference* newObject = self->fullGC.oldLookup[id];
    assert(newObject);

    return newObject->data;
  };
  
  for (int i = 0; i < heap->youngGeneration->sizeInCells; i++) {
    struct object_info* currentObjectInfo = &heap->youngObjects[i];

    if (!currentObjectInfo->isValid)
      continue;
    
    assert(currentObjectInfo->type != OBJECT_TYPE_UNKNOWN);
    gc_fix_object_refs_custom(self, currentObjectInfo, fixer);
  }
  
  for (int i = 0; i < heap->oldGeneration->sizeInCells; i++) {
    struct object_info* currentObjectInfo = &heap->oldObjects[i];
    if (!currentObjectInfo->isValid)
      continue;
    
    // Fix move data
    if (currentObjectInfo->justMoved) {
      struct object_info* oldInfo = currentObjectInfo->moveData.oldLocationInfo;
      assert(oldInfo->isMoved);

      oldInfo->moveData.newLocation = currentObjectInfo->regionRef;
      oldInfo->moveData.newLocationInfo = currentObjectInfo;
    }

    assert(currentObjectInfo->type != OBJECT_TYPE_UNKNOWN);
    gc_fix_object_refs_custom(self, currentObjectInfo, fixer);
  }
}

static void fixRegistry(struct gc_state* self) {
  struct heap* heap = self->heap;  
  
  for (int i = 0; i < heap->oldGeneration->sizeInCells; i++) {
    struct object_info* currentObjectInfo = &heap->oldObjects[i];
    if (!currentObjectInfo->isValid || 
        currentObjectInfo->regionRef->id == i) {
      continue;
    }
    
    int newLocation = currentObjectInfo->regionRef->id;
    
    // In theory it shouldnt overlap anything
    // except itself
    struct object_info oldObjectInfo = heap->oldObjects[newLocation];
    if (oldObjectInfo.isValid && oldObjectInfo.regionRef != currentObjectInfo->regionRef) 
      abort();

    heap->oldObjects[newLocation] = *currentObjectInfo;
    heap_reset_object_info(heap, currentObjectInfo);
    currentObjectInfo->isValid = false;
  }
}

void gc_full_collect(struct gc_state* self) {
  struct heap* heap = self->heap;  
 
  if (self->isExplicit) {
    gc_trigger_young_collection(self, REPORT_FULL_COLLECTION);
    gc_trigger_old_collection(self, REPORT_FULL_COLLECTION);
  }

  // Record previous locations
  profiler_begin(self->profiler, "record-refs");
  for (int i = 0; i < heap->oldGeneration->sizeInCells; i++) {
    struct object_info* currentObjectInfo = &heap->oldObjects[i];
    if (!currentObjectInfo->isValid)
      continue;
    
    assert(self->fullGC.oldLookup[region_get_cellid(heap->oldGeneration, currentObjectInfo->regionRef->data)] == NULL ||
           self->fullGC.oldLookup[region_get_cellid(heap->oldGeneration, currentObjectInfo->regionRef->data)] == currentObjectInfo->regionRef);
    self->fullGC.oldLookup[region_get_cellid(heap->oldGeneration, currentObjectInfo->regionRef->data)] = currentObjectInfo->regionRef;
  }
  profiler_end(self->profiler);
  
  profiler_begin(self->profiler, "compact");
  region_compact(self->heap->oldGeneration);
  profiler_end(self->profiler);
 
  // Fix the objects registry
  // for old generation
  profiler_begin(self->profiler, "fix-registry");
  fixRegistry(self); 
  profiler_end(self->profiler);

  // Last fix ups
  profiler_begin(self->profiler, "fix-refs");
  fixRefs(self);
  profiler_end(self->profiler);
}

void gc_full_post_collect(struct gc_state* self) {
  region_move_bump_pointer_to_last(self->heap->oldGeneration);
}

