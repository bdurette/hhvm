/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/cfg.h"
#include <algorithm>
#include "hphp/runtime/vm/jit/id-set.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/mutation.h"

namespace HPHP {  namespace JIT {

TRACE_SET_MOD(hhir);

BlockList rpoSortCfg(const IRUnit& unit) {
  BlockList blocks;
  blocks.reserve(unit.numBlocks());
  postorderWalk(unit,
    [&](Block* block) {
      blocks.push_back(block);
    });

  std::reverse(blocks.begin(), blocks.end());
  assert(blocks.size() <= unit.numBlocks());
  return blocks;
}

BlocksWithIds rpoSortCfgWithIds(const IRUnit& unit) {
  auto ret = BlocksWithIds{rpoSortCfg(unit), {unit, 0xffffffff}};

  auto id = ret.blocks.size();
  for (auto* block : ret.blocks) {
    ret.ids[block] = --id;
  }
  assert(id == 0);

  return ret;
}

Block* splitEdge(IRUnit& unit, Block* from, Block* to, BCMarker marker) {
  auto& branch = from->back();
  Block* middle = unit.defBlock();
  FTRACE(3, "splitting edge from B{} -> B{} using B{}\n",
         from->id(), to->id(), middle->id());
  if (branch.taken() == to) {
    branch.setTaken(middle);
  } else {
    assert(branch.next() == to);
    branch.setNext(middle);
  }

  middle->prepend(unit.gen(Jmp, marker, to));
  auto const unlikely = Block::Hint::Unlikely;
  if (from->hint() == unlikely || to->hint() == unlikely) {
    middle->setHint(unlikely);
  }
  return middle;
}

namespace {

// If edge is critical, split it by inserting an intermediate block.
// A critical edge is an edge from a block with multiple successors to
// a block with multiple predecessors.
void splitCriticalEdge(IRUnit& unit, Edge* edge) {
  if (!edge) return;

  auto* to = edge->to();
  auto* branch = edge->inst();
  auto* from = branch->block();
  if (to->numPreds() <= 1 || from->numSuccs() <= 1) return;

  splitEdge(unit, from, to, to->front().marker());
}
}

bool splitCriticalEdges(IRUnit& unit) {
  FTRACE(2, "splitting critical edges\n");
  auto modified = removeUnreachable(unit);
  if (modified) reflowTypes(unit);
  auto const startBlocks = unit.numBlocks();

  // Try to split outgoing edges of each reachable block.  This is safe in
  // a postorder walk since we visit blocks after visiting successors.
  postorderWalk(unit, [&](Block* b) {
    splitCriticalEdge(unit, b->takenEdge());
    splitCriticalEdge(unit, b->nextEdge());
  });

  return modified || unit.numBlocks() != startBlocks;
}

bool removeUnreachable(IRUnit& unit) {
  ITRACE(2, "removing unreachable blocks\n");
  Trace::Indent _i;

  smart::hash_set<Block*, pointer_hash<Block>> visited;
  smart::stack<Block*> stack;
  stack.push(unit.entry());

  // Find all blocks reachable from the entry block.
  while (!stack.empty()) {
    auto* b = stack.top();
    stack.pop();
    if (visited.count(b)) continue;

    visited.insert(b);
    if (auto* taken = b->taken()) {
      if (!visited.count(taken)) stack.push(taken);
    }
    if (auto* next = b->next()) {
      if (!visited.count(next)) stack.push(next);
    }
  }

  // Walk through the reachable blocks and erase any preds that weren't
  // found.
  bool modified = false;
  for (auto* block : visited) {
    auto& preds = block->preds();
    for (auto it = preds.begin(); it != preds.end(); ) {
      auto* inst = it->inst();
      ++it;

      if (!visited.count(inst->block())) {
        ITRACE(3, "removing unreachable B{}\n", inst->block()->id());
        inst->setNext(nullptr);
        inst->setTaken(nullptr);
        modified = true;
      }
    }
  }

  return modified;
}

/*
 * Find the immediate dominator of each block using Cooper, Harvey, and
 * Kennedy's "A Simple, Fast Dominance Algorithm", returned as a vector
 * of Block*, indexed by block.  IdomVector[b] == nullptr if b has no
 * dominator.  This is the case for the entry block and any blocks not
 * reachable from the entry block.
 */
IdomVector findDominators(const IRUnit& unit, const BlocksWithIds& blockIds) {
  auto& blocks = blockIds.blocks;
  auto& postIds = blockIds.ids;

  // Calculate immediate dominators with the iterative two-finger algorithm.
  // When it terminates, idom[post-id] will contain the post-id of the
  // immediate dominator of each block.  idom[start] will be -1.  This is
  // the general algorithm but it will only loop twice for loop-free graphs.
  IdomVector idom(unit, nullptr);
  auto start = blocks.begin();
  auto entry = *start;
  idom[entry] = entry;
  start++;
  for (bool changed = true; changed; ) {
    changed = false;
    // for each block after start, in reverse postorder
    for (auto it = start; it != blocks.end(); it++) {
      Block* block = *it;
      // p1 = any already-processed predecessor
      auto predIter = block->preds().begin();
      auto predEnd = block->preds().end();
      auto p1 = predIter->inst()->block();
      while (!idom[p1]) p1 = (++predIter)->inst()->block();
      // for all other already-processed predecessors p2 of block
      for (++predIter; predIter != predEnd; ++predIter) {
        auto p2 = predIter->inst()->block();
        if (p2 == p1 || !idom[p2]) continue;
        // find earliest common predecessor of p1 and p2
        // (higher postIds are earlier in flow and in dom-tree).
        do {
          while (postIds[p1] < postIds[p2]) p1 = idom[p1];
          while (postIds[p2] < postIds[p1]) p2 = idom[p2];
        } while (p1 != p2);
      }
      if (idom[block] != p1) {
        idom[block] = p1;
        changed = true;
      }
    }
  }
  idom[entry] = nullptr; // entry has no dominator.
  return idom;
}

DomChildren findDomChildren(const IRUnit& unit, const BlocksWithIds& blocks) {
  IdomVector idom = findDominators(unit, blocks);
  DomChildren children(unit, BlockList());
  for (Block* block : blocks.blocks) {
    auto idomBlock = idom[block];
    if (idomBlock) children[idomBlock].push_back(block);
  }
  return children;
}

bool dominates(const Block* b1, const Block* b2, const IdomVector& idoms) {
  for (auto b = b2; b != nullptr; b = idoms[b]) {
    if (b == b1) return true;
  }
  return false;
}

static bool loopVisit(const Block* b,
                      boost::dynamic_bitset<>& visited,
                      boost::dynamic_bitset<>& path) {
  if (b == nullptr) return false;

  auto const id = b->id();

  // If we're revisiting a block in our current search, then we've
  // found a backedge.
  if (path.test(id)) return true;

  // Otherwise if we're getting back to a block that's already been
  // visited, but it hasn't been visited in this path, then we can
  // prune this search.
  if (visited.test(id)) return false;

  visited.set(id);
  path.set(id);

  bool res = loopVisit(b->taken(), visited, path) ||
             loopVisit(b->next(), visited, path);

  path.set(id, false);

  return res;
}

bool cfgHasLoop(const IRUnit& unit) {
  boost::dynamic_bitset<> path(unit.numBlocks());
  boost::dynamic_bitset<> visited(unit.numBlocks());
  return loopVisit(unit.entry(), path, visited);
}

}}
