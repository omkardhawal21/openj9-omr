/*******************************************************************************
 * Copyright (c) 2000, 2021 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "optimizer/Optimizer.hpp"

#include <iostream>
//#include "ptgparser/structs.h"
#include "invariantparser/parser/PointsToGraph.h"
#include <map>
#include <queue>
#include "il/ParameterSymbol.hpp"
using namespace std;
#include "optimizer/Optimizer_inlines.hpp"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "codegen/CodeGenerator.hpp"
#include "env/FrontEnd.hpp"
#include "compile/Compilation.hpp"
#include "compile/CompilationTypes.hpp"
#include "compile/Method.hpp"
#include "compile/SymbolReferenceTable.hpp"
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "control/Recompilation.hpp"
#ifdef J9_PROJECT_SPECIFIC
#include "control/RecompilationInfo.hpp"
#endif
#include "env/CompilerEnv.hpp"
#include "env/IO.hpp"
#include "env/PersistentInfo.hpp"
#include "env/StackMemoryRegion.hpp"
#include "env/TRMemory.hpp"
#include "env/jittypes.h"
#include "il/Block.hpp"
#include "il/DataTypes.hpp"
#include "il/ILOpCodes.hpp"
#include "il/ILOps.hpp"
#include "il/Node.hpp"
#include "il/NodePool.hpp"
#include "il/Node_inlines.hpp"
#include "il/ResolvedMethodSymbol.hpp"
#include "il/Symbol.hpp"
#include "il/SymbolReference.hpp"
#include "il/TreeTop.hpp"
#include "il/TreeTop_inlines.hpp"
#include "infra/Assert.hpp"
#include "infra/BitVector.hpp"
#include "infra/Cfg.hpp"
#include "infra/List.hpp"
#include "infra/SimpleRegex.hpp"
#include "infra/CfgNode.hpp"
#include "infra/Timer.hpp"
#include "optimizer/LoadExtensions.hpp"
#include "optimizer/Optimization.hpp"
#include "optimizer/OptimizationManager.hpp"
#include "optimizer/OptimizationStrategies.hpp"
#include "optimizer/Optimizations.hpp"
#include "optimizer/Structure.hpp"
#include "optimizer/StructuralAnalysis.hpp"
#include "optimizer/UseDefInfo.hpp"
#include "optimizer/ValueNumberInfo.hpp"
#include "optimizer/AsyncCheckInsertion.hpp"
#include "optimizer/DeadStoreElimination.hpp"
#include "optimizer/DeadTreesElimination.hpp"
#include "optimizer/CatchBlockRemover.hpp"
#include "optimizer/CFGSimplifier.hpp"
#include "optimizer/CompactLocals.hpp"
#include "optimizer/CopyPropagation.hpp"
#include "optimizer/ExpressionsSimplification.hpp"
#include "optimizer/GeneralLoopUnroller.hpp"
#include "optimizer/LocalCSE.hpp"
#include "optimizer/LocalDeadStoreElimination.hpp"
#include "optimizer/LocalLiveRangeReducer.hpp"
#include "optimizer/LocalOpts.hpp"
#include "optimizer/LocalReordering.hpp"
#include "optimizer/LoopCanonicalizer.hpp"
#include "optimizer/LoopReducer.hpp"
#include "optimizer/LoopReplicator.hpp"
#include "optimizer/LoopVersioner.hpp"
#include "optimizer/OrderBlocks.hpp"
#include "optimizer/RedundantAsyncCheckRemoval.hpp"
#include "optimizer/Simplifier.hpp"
#include "optimizer/VirtualGuardCoalescer.hpp"
#include "optimizer/VirtualGuardHeadMerger.hpp"
#include "optimizer/Inliner.hpp"
#include "ras/Debug.hpp"
#include "optimizer/InductionVariable.hpp"
#include "optimizer/GlobalValuePropagation.hpp"
#include "optimizer/LocalValuePropagation.hpp"
#include "optimizer/RegDepCopyRemoval.hpp"
#include "optimizer/SinkStores.hpp"
#include "optimizer/PartialRedundancy.hpp"
#include "optimizer/OSRDefAnalysis.hpp"
#include "optimizer/StripMiner.hpp"
#include "optimizer/FieldPrivatizer.hpp"
#include "optimizer/ReorderIndexExpr.hpp"
#include "optimizer/GlobalRegisterAllocator.hpp"
#include "optimizer/RecognizedCallTransformer.hpp"
#include "optimizer/SwitchAnalyzer.hpp"
#include "env/RegionProfiler.hpp"
//#include "il/ParameterSymbol.hpp"

static std::set<string> _runtimeVerifiedMethods;
static std::map<string, int> _methodIndices;
static bool _runtimeVerifierDiagnostics;
static TR::Compilation *_runtimeVerifierComp;
std::map<string, PointsToGraph *> forceCallsiteArgsForJITCInvocation;
std::map <string, PointsToGraph *> verifiedMethodSummaries;

#define IFDIAGPRINT                 \
   if (_runtimeVerifierDiagnostics) \
   cout
#define INVARIANT_DIR "invariants/"

namespace TR
{
   class AutomaticSymbol;
}

using namespace OMR; // Note: used here only to avoid having to prepend all opts in strategies with OMR::

#define MAX_LOCAL_OPTS_ITERS 5

extern map<string, int> readMethodIndices();
extern map<int, PointsToGraph> readLoopInvariant(int methodIndex);
extern PointsToGraph readCallsiteInvariant(int methodIndex);

const OptimizationStrategy localValuePropagationOpts[] =
    {
        {localCSE},
        {localValuePropagation},
        {localCSE, IfEnabled},
        {localValuePropagation, IfEnabled},
        {endGroup}};

const OptimizationStrategy arrayPrivatizationOpts[] =
    {
        {globalValuePropagation, IfMoreThanOneBlock},      // reduce # of null/bounds checks and setup iv info
        {veryCheapGlobalValuePropagationGroup, IfEnabled}, // enabled by blockVersioner
        {inductionVariableAnalysis, IfLoops},
        {loopCanonicalization, IfLoops}, // setup for any unrolling in arrayPrivatization
        {treeSimplification},            // get rid of null/bnd checks if possible
        {deadTreesElimination},
        {basicBlockOrdering, IfLoops}, // required for loop reduction
        {treesCleansing, IfLoops},
        {inductionVariableAnalysis, IfLoops},                   // required for array Privatization
        {basicBlockOrdering, IfEnabled},                        // cleanup if unrolling happened
        {globalValuePropagation, IfEnabledAndMoreThanOneBlock}, // ditto
        {endGroup}};

// To be run just before PRE
const OptimizationStrategy reorderArrayIndexOpts[] =
    {
        {inductionVariableAnalysis, IfLoops}, // need to id the primary IVs
        {reorderArrayIndexExpr, IfLoops},     // try to maximize loop invarient
                                              // expressions in index calculations and be hoisted
        {endGroup}};

const OptimizationStrategy cheapObjectAllocationOpts[] =
    {
        {eachEscapeAnalysisPassGroup, IfEAOpportunitiesAndNotOptServer},
        {explicitNewInitialization, IfNews}, // do before local dead store
                                             // basicBlockHoisting,                     // merge block into pred and prepare for local dead store
        {localDeadStoreElimination},         // remove local/parm/some field stores
        {endGroup}};

const OptimizationStrategy expensiveObjectAllocationOpts[] =
    {
        {eachEscapeAnalysisPassGroup, IfEAOpportunities},
        {explicitNewInitialization, IfNews}, // do before local dead store
        {endGroup}};

const OptimizationStrategy eachEscapeAnalysisPassOpts[] =
    {
        {preEscapeAnalysis, IfOSR},
        {escapeAnalysis},
        {postEscapeAnalysis, IfOSR},
        {eachEscapeAnalysisPassGroup, IfEnabled}, // if another pass requested
        {endGroup}};

const OptimizationStrategy veryCheapGlobalValuePropagationOpts[] =
    {
        {globalValuePropagation, IfMoreThanOneBlock},
        {endGroup}};

const OptimizationStrategy cheapGlobalValuePropagationOpts[] =
    {
        //{ catchBlockRemoval,                     },
        {CFGSimplification, IfOptServer},            // for WAS trace folding
        {treeSimplification, IfOptServer},           // for WAS trace folding
        {localCSE, IfEnabledAndOptServer},           // for WAS trace folding
        {treeSimplification, IfEnabledAndOptServer}, // for WAS trace folding
        {globalValuePropagation, IfMoreThanOneBlock},
        {localValuePropagation, IfOneBlock},
        {treeSimplification, IfEnabled},
        {cheapObjectAllocationGroup},
        {globalValuePropagation, IfEnabled}, // if inlined a call or an object
        {treeSimplification, IfEnabled},
        {catchBlockRemoval, IfEnabled},                                    // if checks were removed
        {osrExceptionEdgeRemoval},                                         // most inlining is done by now
        {redundantMonitorElimination, IfMonitors},                         // performed if method has monitors
        {redundantMonitorElimination, IfEnabledAndMonitors},               // performed if method has monitors
        {globalValuePropagation, IfEnabledAndMoreThanOneBlockMarkLastRun}, // mark monitors requiring sync
        {virtualGuardTailSplitter, IfEnabled},                             // merge virtual guards
        {CFGSimplification},
        {endGroup}};

const OptimizationStrategy expensiveGlobalValuePropagationOpts[] =
    {
        /////   { innerPreexistence                             },
        {CFGSimplification, IfOptServer},  // for WAS trace folding
        {treeSimplification, IfOptServer}, // for WAS trace folding
        {localCSE, IfEnabledAndOptServer}, // for WAS trace folding
        {treeSimplification, IfEnabled},   // may be enabled by inner prex
        {globalValuePropagation, IfMoreThanOneBlock},
        {treeSimplification, IfEnabled},
        {deadTreesElimination}, // clean up left-over accesses before escape analysis
#ifdef J9_PROJECT_SPECIFIC
        {expensiveObjectAllocationGroup},
#endif
        {globalValuePropagation, IfEnabledAndMoreThanOneBlock}, // if inlined a call or an object
        {treeSimplification, IfEnabled},
        {catchBlockRemoval, IfEnabled}, // if checks were removed
        {osrExceptionEdgeRemoval},      // most inlining is done by now
#ifdef J9_PROJECT_SPECIFIC
        {redundantMonitorElimination, IfEnabled}, // performed if method has monitors
        {redundantMonitorElimination, IfEnabled}, // performed if method has monitors
#endif
        {globalValuePropagation, IfEnabledAndMoreThanOneBlock}, // mark monitors requiring sync
        {virtualGuardTailSplitter, IfEnabled},                  // merge virtual guards
        {CFGSimplification},
        {endGroup}};

const OptimizationStrategy eachExpensiveGlobalValuePropagationOpts[] =
    {
        //{ blockSplitter                                        },
        ///   { innerPreexistence                                      },
        {globalValuePropagation, IfMoreThanOneBlock},
        {treeSimplification, IfEnabled},
        {veryCheapGlobalValuePropagationGroup, IfEnabled}, // enabled by blockversioner
        {deadTreesElimination},                            // clean up left-over accesses before escape analysis
#ifdef J9_PROJECT_SPECIFIC
        {expensiveObjectAllocationGroup},
#endif
        {eachExpensiveGlobalValuePropagationGroup, IfEnabled}, // if inlining was done
        {endGroup}};

const OptimizationStrategy veryExpensiveGlobalValuePropagationOpts[] =
    {
        {eachExpensiveGlobalValuePropagationGroup},
        //{ basicBlockHoisting,                           }, // merge block into pred and prepare for local dead store
        {localDeadStoreElimination}, // remove local/parm/some field stores
        {treeSimplification, IfEnabled},
        {catchBlockRemoval, IfEnabled}, // if checks were removed
        {osrExceptionEdgeRemoval},      // most inlining is done by now
#ifdef J9_PROJECT_SPECIFIC
        {redundantMonitorElimination, IfEnabled}, // performed if method has monitors
        {redundantMonitorElimination, IfEnabled}, // performed if method has monitors
#endif
        {globalValuePropagation, IfEnabledAndMoreThanOneBlock}, // mark monitors requiring syncs
        {virtualGuardTailSplitter, IfEnabled},                  // merge virtual guards
        {CFGSimplification},
        {endGroup}};

const OptimizationStrategy partialRedundancyEliminationOpts[] =
    {
        {globalValuePropagation, IfMoreThanOneBlock}, // GVP (before PRE)
        {deadTreesElimination},
        {treeSimplification, IfEnabled},
        {treeSimplification},               // might fold expressions created by versioning/induction variables
        {treeSimplification, IfEnabled},    // Array length simplification shd be followed by reassoc before PRE
        {reorderArrayExprGroup, IfEnabled}, // maximize opportunities hoisting of index array expressions
        {partialRedundancyElimination, IfMoreThanOneBlock},
        {
            localCSE,
        },                                                                 // common up expression which can benefit EA
        {catchBlockRemoval, IfEnabled},                                    // if checks were removed
        {deadTreesElimination, IfEnabled},                                 // if checks were removed
        {compactNullChecks, IfEnabled},                                    // PRE creates explicit null checks in large numbers
        {localReordering, IfEnabled},                                      // PRE may create temp stores that can be moved closer to uses
        {globalValuePropagation, IfEnabledAndMoreThanOneBlockMarkLastRun}, // GVP (after PRE)
#ifdef J9_PROJECT_SPECIFIC
        {preEscapeAnalysis, IfOSR},
        {escapeAnalysis, IfEAOpportunitiesMarkLastRun}, // to stack-allocate after loopversioner and localCSE
        {postEscapeAnalysis, IfOSR},
#endif
        {basicBlockOrdering, IfLoops},    // early ordering with no extension
        {globalCopyPropagation, IfLoops}, // for Loop Versioner

        {loopVersionerGroup, IfEnabledAndLoops},
        {treeSimplification, IfEnabled},                            // loop reduction block should be after PRE so that privatization
        {treesCleansing},                                           // clean up gotos in code and convert to fall-throughs for loop reducer
        {redundantGotoElimination, IfNotJitProfiling},              // clean up for loop reducer.  Note: NEVER run this before PRE
        {loopReduction, IfLoops},                                   // will have happened and it needs to be before loopStrider
        {localCSE, IfEnabled},                                      // so that it will not get confused with internal pointers.
        {globalDeadStoreElimination, IfEnabledAndMoreThanOneBlock}, // It may need to be run twice if deadstore elimination is required,
        {
            deadTreesElimination,
        }, // but this only happens for unsafe access (arraytranslate.twoToOne)
        {
            loopReduction,
        }, // and so is conditional
#ifdef J9_PROJECT_SPECIFIC
        {idiomRecognition, IfLoopsAndNotProfiling}, // after loopReduction!!
#endif
        {lastLoopVersionerGroup, IfLoops},
        {
            treeSimplification,
        }, // cleanup before AutoVectorization
        {
            deadTreesElimination,
        }, // cleanup before AutoVectorization
        {inductionVariableAnalysis, IfLoopsAndNotProfiling},
#ifdef J9_PROJECT_SPECIFIC
        {SPMDKernelParallelization, IfLoops},
#endif
        {loopStrider, IfLoops},
        {treeSimplification, IfEnabled},
        {lastLoopVersionerGroup, IfEnabledAndLoops},
        {
            treeSimplification,
        }, // cleanup before strider
        {
            localCSE,
        }, // cleanup before strider so it will not be confused by commoned nodes (mandatory to run local CSE before strider)
        {
            deadTreesElimination,
        }, // cleanup before strider so that dead stores can be eliminated more effcientlly (i.e. false uses are not seen)
        {loopStrider, IfLoops},

        {treeSimplification, IfEnabled}, // cleanup after strider
        {loopInversion, IfLoops},
        {endGroup}};

const OptimizationStrategy methodHandleInvokeInliningOpts[] =
    {
        {
            treeSimplification,
        }, // Supply some known-object info, and help CSE
        {
            localCSE,
        },                       // Especially copy propagation to replace temps with more descriptive trees
        {localValuePropagation}, // Propagate known-object info and derive more specific archetype specimen symbols for inlining
#ifdef J9_PROJECT_SPECIFIC
        {
            targetedInlining,
        },
#endif
        {deadTreesElimination},
        {methodHandleInvokeInliningGroup, IfEnabled}, // Repeat as required to inline all the MethodHandle.invoke calls we can afford
        {endGroup},
};

const OptimizationStrategy earlyGlobalOpts[] =
    {
        {methodHandleInvokeInliningGroup, IfMethodHandleInvokes},
#ifdef J9_PROJECT_SPECIFIC
        {inlining},
#endif
        {osrExceptionEdgeRemoval}, // most inlining is done by now
        //{ basicBlockOrdering,          IfLoops }, // early ordering with no extension
        {treeSimplification, IfEnabled},
        {compactNullChecks}, // cleans up after inlining; MUST be done before PRE
#ifdef J9_PROJECT_SPECIFIC
        {virtualGuardTailSplitter}, // merge virtual guards
        {treeSimplification},
        {CFGSimplification},
#endif
        {endGroup}};

const OptimizationStrategy earlyLocalOpts[] =
    {
        {localValuePropagation},
        //{ localValuePropagationGroup           },
        {localReordering},
        {
            switchAnalyzer,
        },
        {treeSimplification, IfEnabled}, // simplify any exprs created by LCP/LCSE
#ifdef J9_PROJECT_SPECIFIC
        {catchBlockRemoval}, // if all possible exceptions in a try were removed by inlining/LCP/LCSE
#endif
        {deadTreesElimination}, // remove any anchored dead loads
        {profiledNodeVersioning},
        {endGroup}};

const OptimizationStrategy isolatedStoreOpts[] =
    {
        {isolatedStoreElimination},
        {deadTreesElimination},
        {endGroup}};

const OptimizationStrategy globalDeadStoreOpts[] =
    {
        {globalDeadStoreElimination, IfMoreThanOneBlock},
        {deadTreesElimination},
        {endGroup}};

const OptimizationStrategy loopAliasRefinerOpts[] =
    {
        {inductionVariableAnalysis, IfLoops},
        {loopCanonicalization},
        {globalValuePropagation, IfMoreThanOneBlock}, // create ivs
        {loopAliasRefiner},
        {endGroup}};

const OptimizationStrategy loopSpecializerOpts[] =
    {
        {inductionVariableAnalysis, IfLoops},
        {loopCanonicalization},
        {loopSpecializer},
        {endGroup}};

const OptimizationStrategy loopVersionerOpts[] =
    {
        {basicBlockOrdering},
        {inductionVariableAnalysis, IfLoops},
        {loopCanonicalization},
        {loopVersioner},
        {endGroup}};

const OptimizationStrategy lastLoopVersionerOpts[] =
    {
        {inductionVariableAnalysis, IfLoops},
        {loopCanonicalization},
        {loopVersioner, MarkLastRun},
        {endGroup}};

const OptimizationStrategy loopCanonicalizationOpts[] =
    {
        {globalCopyPropagation, IfLoops}, // propagate copies to allow better invariance detection
        {loopVersionerGroup},
        {deadTreesElimination}, // remove dead anchors created by check removal (versioning)
        //{ loopStrider                        }, // use canonicalized loop to insert initializations
        {treeSimplification},                      // remove unreachable blocks (with nullchecks etc.) left by LoopVersioner
        {fieldPrivatization},                      // use canonicalized loop to privatize fields
        {treeSimplification},                      // might fold expressions created by versioning/induction variables
        {loopSpecializerGroup, IfEnabledAndLoops}, // specialize the versioned loop if possible
        {deadTreesElimination, IfEnabledAndLoops}, // remove dead anchors created by specialization
        {treeSimplification, IfEnabledAndLoops},   // might fold expressions created by specialization
        {endGroup}};

const OptimizationStrategy stripMiningOpts[] =
    {
        {inductionVariableAnalysis, IfLoops},
        {loopCanonicalization},
        {inductionVariableAnalysis},
        {stripMining},
        {endGroup}};

const OptimizationStrategy blockManipulationOpts[] =
    {
        //   { generalLoopUnroller,       IfLoops   }, //Unroll Loops
        {coldBlockOutlining},
        {CFGSimplification, IfNotJitProfiling},
        {basicBlockHoisting, IfNotJitProfiling},
        {treeSimplification},
        {redundantGotoElimination, IfNotJitProfiling}, // redundant gotos gone
        {treesCleansing},                              // maximize fall throughs
        {virtualGuardHeadMerger},
        {basicBlockExtension, MarkLastRun}, // extend blocks; move trees around if reqd
        {treeSimplification},               // revisit; not really required ?
        {basicBlockPeepHole, IfEnabled},
        {endGroup}};

const OptimizationStrategy eachLocalAnalysisPassOpts[] =
    {
        {localValuePropagationGroup, IfEnabled},
#ifdef J9_PROJECT_SPECIFIC
        {arraycopyTransformation},
#endif
        {treeSimplification, IfEnabled},
        {localCSE, IfEnabled},
        {localDeadStoreElimination, IfEnabled}, // after local copy/value propagation
        {rematerialization, IfEnabled},
        {compactNullChecks, IfEnabled},
        {deadTreesElimination, IfEnabled}, // remove dead anchors created by check/store removal
        //{ eachLocalAnalysisPassGroup, IfEnabled  }, // if another pass requested
        {endGroup}};

const OptimizationStrategy lateLocalOpts[] =
    {
        {OMR::eachLocalAnalysisPassGroup},
        {OMR::andSimplification}, // needs commoning across blocks to work well; must be done after versioning
        {OMR::treesCleansing},    // maximize fall throughs after LCP has converted some conditions to gotos
        {OMR::eachLocalAnalysisPassGroup},
        {OMR::localDeadStoreElimination}, // after latest copy propagation
        {OMR::deadTreesElimination},      // remove dead anchors created by check/store removal
        {
            OMR::globalDeadStoreGroup,
        },
        {OMR::eachLocalAnalysisPassGroup},
        {OMR::treeSimplification},
        {OMR::endGroup}};

static const OptimizationStrategy tacticalGlobalRegisterAllocatorOpts[] =
    {
        {OMR::inductionVariableAnalysis, OMR::IfLoops},
        {OMR::loopCanonicalization, OMR::IfLoops},
        {OMR::liveRangeSplitter, OMR::IfLoops},
        {OMR::redundantGotoElimination, OMR::IfNotJitProfiling}, // need to be run before global register allocator
        {OMR::treeSimplification, OMR::MarkLastRun},             // Cleanup the trees after redundantGotoElimination
        {OMR::tacticalGlobalRegisterAllocator, OMR::IfEnabled},
        {OMR::localCSE},
        // { isolatedStoreGroup,                    IfEnabled                    }, // if global register allocator created stores from registers
        {OMR::globalCopyPropagation, OMR::IfEnabledAndMoreThanOneBlock}, // if live range splitting created copies
        {OMR::localCSE},                                                 // localCSE after post-PRE + post-GRA globalCopyPropagation to clean up whole expression remat (rtc 64659)
        {OMR::globalDeadStoreGroup, OMR::IfEnabled},
        {OMR::redundantGotoElimination, OMR::IfEnabledAndNotJitProfiling}, // if global register allocator created new block
        {OMR::deadTreesElimination},                                       // remove dangling GlRegDeps
        {OMR::deadTreesElimination, OMR::IfEnabled},                       // remove dead RegStores produced by previous deadTrees pass
        {OMR::deadTreesElimination, OMR::IfEnabled},                       // remove dead RegStores produced by previous deadTrees pass
        {OMR::endGroup}};

const OptimizationStrategy finalGlobalOpts[] =
    {
        {rematerialization},
        {compactNullChecks, IfEnabled},
        {deadTreesElimination},
        //{ treeSimplification,       IfEnabled  },
        {localLiveRangeReduction},
        {compactLocals, IfNotJitProfiling}, // analysis results are invalidated by profilingGroup
#ifdef J9_PROJECT_SPECIFIC
        {globalLiveVariablesForGC},
#endif
        {endGroup}};

// **************************************************************************
//
// Strategy that is run for each non-peeking IlGeneration - this allows early
// optimizations to be run even before the IL is available to Inliner
//
// **************************************************************************
static const OptimizationStrategy ilgenStrategyOpts[] =
    {
#ifdef J9_PROJECT_SPECIFIC
        {osrLiveRangeAnalysis, IfOSR},
        {osrDefAnalysis, IfInvoluntaryOSR},
        {
            methodHandleTransformer,
        },
        {varHandleTransformer, MustBeDone},
        {handleRecompilationOps, MustBeDone},
        {unsafeFastPath},
        {recognizedCallTransformer},
        {coldBlockMarker},
        {CFGSimplification},
        {allocationSinking, IfNews},
        {invariantArgumentPreexistence, IfNotClassLoadPhaseAndNotProfiling}, // Should not run if a recompilation is possible
#endif
        {endOpts},
};

// **********************************************************
//
// OMR Strategies
//
// **********************************************************

static const OptimizationStrategy omrNoOptStrategyOpts[] =
    {
        {endOpts},
};

static const OptimizationStrategy omrColdStrategyOpts[] =
    {
        {basicBlockExtension},
        {localCSE},
        //{ localValuePropagation                },
        {treeSimplification},
        {localCSE},
        {endOpts},
};

static const OptimizationStrategy omrWarmStrategyOpts[] =
    {
        {basicBlockExtension},
        {localCSE},
        //{ localValuePropagation               },
        {treeSimplification},
        {localCSE},
        {localDeadStoreElimination},
        {globalDeadStoreGroup},
        {endOpts},
};

static const OptimizationStrategy omrHotStrategyOpts[] =
    {
        {OMR::coldBlockOutlining},
        {OMR::earlyGlobalGroup},
        {OMR::earlyLocalGroup},
        {OMR::andSimplification}, // needs commoning across blocks to work well; must be done after versioning
        {
            OMR::stripMiningGroup,
        }, // strip mining in loops
        {
            OMR::loopReplicator,
        }, // tail-duplication in loops
        {
            OMR::blockSplitter,
        }, // treeSimplification + blockSplitter + VP => opportunity for EA
        {
            OMR::arrayPrivatizationGroup,
        }, // must preceed escape analysis
        {OMR::veryExpensiveGlobalValuePropagationGroup},
        {
            OMR::globalDeadStoreGroup,
        },
        {
            OMR::globalCopyPropagation,
        },
        {
            OMR::loopCanonicalizationGroup,
        }, // canonicalize loops (improve fall throughs)
        {
            OMR::expressionsSimplification,
        },
        {OMR::partialRedundancyEliminationGroup},
        {
            OMR::globalDeadStoreElimination,
        },
        {
            OMR::inductionVariableAnalysis,
        },
        {
            OMR::loopSpecializerGroup,
        },
        {
            OMR::inductionVariableAnalysis,
        },
        {
            OMR::generalLoopUnroller,
        }, // unroll Loops
        {OMR::blockSplitter, OMR::MarkLastRun},
        {OMR::blockManipulationGroup},
        {OMR::lateLocalGroup},
        {OMR::redundantAsyncCheckRemoval}, // optimize async check placement
#ifdef J9_PROJECT_SPECIFIC
        {
            OMR::recompilationModifier,
        }, // do before GRA to avoid commoning of longs afterwards
#endif
        {
            OMR::globalCopyPropagation,
        }, // Can produce opportunities for store sinking
        {OMR::generalStoreSinking},
        {
            OMR::localCSE,
        }, // common up lit pool refs in the same block
        {
            OMR::treeSimplification,
        }, // cleanup the trees after sunk store and localCSE
        {OMR::trivialBlockExtension},
        {
            OMR::localDeadStoreElimination,
        }, // remove the astore if no literal pool is required
        {
            OMR::localCSE,
        }, // common up lit pool refs in the same block
        {OMR::arraysetStoreElimination},
        {OMR::localValuePropagation, OMR::MarkLastRun},
        {OMR::checkcastAndProfiledGuardCoalescer},
        {OMR::osrExceptionEdgeRemoval, OMR::MarkLastRun},
        {
            OMR::tacticalGlobalRegisterAllocatorGroup,
        },
        {
            OMR::globalDeadStoreElimination,
        },                           // global dead store removal
        {OMR::deadTreesElimination}, // cleanup after dead store removal
        {OMR::compactNullChecks},    // cleanup at the end
        {OMR::finalGlobalGroup},     // done just before codegen
        {OMR::regDepCopyRemoval},
        {endOpts},
};

// The following arrays of Optimization pointers are externally declared in OptimizerStrategies.hpp
// This allows frontends to assist in selection of optimizer strategies.
// (They cannot be made 'static const')

const OptimizationStrategy *omrCompilationStrategies[] =
    {
        omrNoOptStrategyOpts, // empty strategy
        omrColdStrategyOpts,  // <<  specialized
        omrWarmStrategyOpts,  // <<  specialized
        omrHotStrategyOpts,   // currently used to test available omr optimizations
};

#ifdef OPT_TIMING // provide statistics on time taken by individual optimizations
TR_Stats statOptTiming[OMR::numOpts];
TR_Stats statStructuralAnalysisTiming("Structural Analysis");
TR_Stats statUseDefsTiming("Use Defs");
TR_Stats statGlobalValNumTiming("Global Value Numbering");
#endif // OPT_TIMING

TR::Optimizer *OMR::Optimizer::createOptimizer(TR::Compilation *comp, TR::ResolvedMethodSymbol *methodSymbol, bool isIlGen)
{
   // returns IL optimizer, performs tree-to-tree optimizing transformations.
   if (isIlGen)
      return new (comp->trHeapMemory()) TR::Optimizer(comp, methodSymbol, isIlGen, ilgenStrategyOpts);

   if (comp->getOptions()->getCustomStrategy())
   {
      if (comp->getOption(TR_TraceOptDetails))
         traceMsg(comp, "Using custom optimization strategy\n");

      // Reformat custom strategy as array of Optimization rather than array of int32_t
      //
      int32_t *srcStrategy = comp->getOptions()->getCustomStrategy();
      int32_t size = comp->getOptions()->getCustomStrategySize();
      OptimizationStrategy *customStrategy = (OptimizationStrategy *)comp->trMemory()->allocateHeapMemory(size * sizeof(customStrategy[0]));
      for (int32_t i = 0; i < size; i++)
      {
         OptimizationStrategy o = {(OMR::Optimizations)(srcStrategy[i] & TR::Options::OptNumMask)};
         if (srcStrategy[i] & TR::Options::MustBeDone)
            o._options = MustBeDone;
         customStrategy[i] = o;
      }

      return new (comp->trHeapMemory()) TR::Optimizer(comp, methodSymbol, isIlGen, customStrategy);
   }

   TR::Optimizer *optimizer = new (comp->trHeapMemory()) TR::Optimizer(
       comp,
       methodSymbol,
       isIlGen,
       TR::Optimizer::optimizationStrategy(comp),
       TR::Optimizer::valueNumberInfoBuildType());

   return optimizer;
}

// ************************************************************************
//
// Implementation of TR::Optimizer
//
// ************************************************************************

OMR::Optimizer::Optimizer(TR::Compilation *comp, TR::ResolvedMethodSymbol *methodSymbol, bool isIlGen,
                          const OptimizationStrategy *strategy, uint16_t VNType)
    : _compilation(comp),
      _cg(comp->cg()),
      _trMemory(comp->trMemory()),
      _methodSymbol(methodSymbol),
      _isIlGen(isIlGen),
      _strategy(strategy),
      _vnInfoType(VNType),
      _symReferencesTable(NULL),
      _useDefInfo(NULL),
      _valueNumberInfo(NULL),
      _aliasSetsAreValid(false),
      _cantBuildGlobalsUseDefInfo(false),
      _cantBuildLocalsUseDefInfo(false),
      _cantBuildGlobalsValueNumberInfo(false),
      _cantBuildLocalsValueNumberInfo(false),
      _canRunBlockByBlockOptimizations(true),
      _cachedExtendedBBInfoValid(false),
      _inlineSynchronized(true),
      _enclosingFinallyBlock(NULL),
      _eliminatedCheckcastNodes(comp->trMemory()),
      _classPointerNodes(comp->trMemory()),
      _optMessageIndex(0),
      _seenBlocksGRA(NULL),
      _resetExitsGRA(NULL),
      _successorBitsGRA(NULL),
      _stackedOptimizer(false),
      _firstTimeStructureIsBuilt(true),
      _disableLoopOptsThatCanCreateLoops(false)
{
   // zero opts table
   memset(_opts, 0, sizeof(_opts));

/*
 * Allow downstream projects to disable the default initialization of optimizations
 * and allow them to take full control over this process.  This can be an advantage
 * if they don't use all of the optimizations initialized here as they can avoid
 * getting linked in to the binary in their entirety.
 */
#if !defined(TR_OVERRIDE_OPTIMIZATION_INITIALIZATION)
   // initialize OMR optimizations

   _opts[OMR::andSimplification] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_SimplifyAnds::create, OMR::andSimplification);
   _opts[OMR::arraysetStoreElimination] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_ArraysetStoreElimination::create, OMR::arraysetStoreElimination);
   _opts[OMR::asyncCheckInsertion] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_AsyncCheckInsertion::create, OMR::asyncCheckInsertion);
   _opts[OMR::basicBlockExtension] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_ExtendBasicBlocks::create, OMR::basicBlockExtension);
   _opts[OMR::basicBlockHoisting] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_HoistBlocks::create, OMR::basicBlockHoisting);
   _opts[OMR::basicBlockOrdering] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_OrderBlocks::create, OMR::basicBlockOrdering);
   _opts[OMR::basicBlockPeepHole] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_PeepHoleBasicBlocks::create, OMR::basicBlockPeepHole);
   _opts[OMR::blockShuffling] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_BlockShuffling::create, OMR::blockShuffling);
   _opts[OMR::blockSplitter] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_BlockSplitter::create, OMR::blockSplitter);
   _opts[OMR::catchBlockRemoval] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_CatchBlockRemover::create, OMR::catchBlockRemoval);
   _opts[OMR::CFGSimplification] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::CFGSimplifier::create, OMR::CFGSimplification);
   _opts[OMR::checkcastAndProfiledGuardCoalescer] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_CheckcastAndProfiledGuardCoalescer::create, OMR::checkcastAndProfiledGuardCoalescer);
   _opts[OMR::coldBlockMarker] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_ColdBlockMarker::create, OMR::coldBlockMarker);
   _opts[OMR::coldBlockOutlining] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_ColdBlockOutlining::create, OMR::coldBlockOutlining);
   _opts[OMR::compactLocals] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_CompactLocals::create, OMR::compactLocals);
   _opts[OMR::compactNullChecks] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_CompactNullChecks::create, OMR::compactNullChecks);
   _opts[OMR::deadTreesElimination] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::DeadTreesElimination::create, OMR::deadTreesElimination);
   _opts[OMR::expressionsSimplification] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_ExpressionsSimplification::create, OMR::expressionsSimplification);
   _opts[OMR::generalLoopUnroller] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_GeneralLoopUnroller::create, OMR::generalLoopUnroller);
   _opts[OMR::globalCopyPropagation] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_CopyPropagation::create, OMR::globalCopyPropagation);
   _opts[OMR::globalDeadStoreElimination] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_DeadStoreElimination::create, OMR::globalDeadStoreElimination);
   _opts[OMR::inlining] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_TrivialInliner::create, OMR::inlining);
   _opts[OMR::innerPreexistence] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_InnerPreexistence::create, OMR::innerPreexistence);
   _opts[OMR::invariantArgumentPreexistence] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_InvariantArgumentPreexistence::create, OMR::invariantArgumentPreexistence);
   _opts[OMR::loadExtensions] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LoadExtensions::create, OMR::loadExtensions);
   _opts[OMR::localCSE] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::LocalCSE::create, OMR::localCSE);
   _opts[OMR::localDeadStoreElimination] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::LocalDeadStoreElimination::create, OMR::localDeadStoreElimination);
   _opts[OMR::localLiveRangeReduction] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LocalLiveRangeReduction::create, OMR::localLiveRangeReduction);
   _opts[OMR::localReordering] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LocalReordering::create, OMR::localReordering);
   _opts[OMR::loopCanonicalization] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LoopCanonicalizer::create, OMR::loopCanonicalization);
   _opts[OMR::loopVersioner] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LoopVersioner::create, OMR::loopVersioner);
   _opts[OMR::loopReduction] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LoopReducer::create, OMR::loopReduction);
   _opts[OMR::loopReplicator] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LoopReplicator::create, OMR::loopReplicator);
   _opts[OMR::profiledNodeVersioning] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_ProfiledNodeVersioning::create, OMR::profiledNodeVersioning);
   _opts[OMR::redundantAsyncCheckRemoval] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_RedundantAsyncCheckRemoval::create, OMR::redundantAsyncCheckRemoval);
   _opts[OMR::redundantGotoElimination] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_EliminateRedundantGotos::create, OMR::redundantGotoElimination);
   _opts[OMR::rematerialization] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_Rematerialization::create, OMR::rematerialization);
   _opts[OMR::treesCleansing] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_CleanseTrees::create, OMR::treesCleansing);
   _opts[OMR::treeSimplification] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::Simplifier::create, OMR::treeSimplification);
   _opts[OMR::trivialBlockExtension] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_TrivialBlockExtension::create, OMR::trivialBlockExtension);
   _opts[OMR::trivialDeadTreeRemoval] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_TrivialDeadTreeRemoval::create, OMR::trivialDeadTreeRemoval);
   _opts[OMR::virtualGuardHeadMerger] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_VirtualGuardHeadMerger::create, OMR::virtualGuardHeadMerger);
   _opts[OMR::virtualGuardTailSplitter] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_VirtualGuardTailSplitter::create, OMR::virtualGuardTailSplitter);
   _opts[OMR::generalStoreSinking] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_GeneralSinkStores::create, OMR::generalStoreSinking);
   _opts[OMR::globalValuePropagation] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::GlobalValuePropagation::create, OMR::globalValuePropagation);
   _opts[OMR::localValuePropagation] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::LocalValuePropagation::create, OMR::localValuePropagation);
   _opts[OMR::redundantInductionVarElimination] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_RedundantInductionVarElimination::create, OMR::redundantInductionVarElimination);
   _opts[OMR::partialRedundancyElimination] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_PartialRedundancy::create, OMR::partialRedundancyElimination);
   _opts[OMR::loopInversion] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LoopInverter::create, OMR::loopInversion);
   _opts[OMR::inductionVariableAnalysis] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_InductionVariableAnalysis::create, OMR::inductionVariableAnalysis);
   _opts[OMR::osrExceptionEdgeRemoval] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_OSRExceptionEdgeRemoval::create, OMR::osrExceptionEdgeRemoval);
   _opts[OMR::regDepCopyRemoval] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::RegDepCopyRemoval::create, OMR::regDepCopyRemoval);
   _opts[OMR::stripMining] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_StripMiner::create, OMR::stripMining);
   _opts[OMR::fieldPrivatization] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_FieldPrivatizer::create, OMR::fieldPrivatization);
   _opts[OMR::reorderArrayIndexExpr] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_IndexExprManipulator::create, OMR::reorderArrayIndexExpr);
   _opts[OMR::loopStrider] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LoopStrider::create, OMR::loopStrider);
   _opts[OMR::osrDefAnalysis] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_OSRDefAnalysis::create, OMR::osrDefAnalysis);
   _opts[OMR::osrLiveRangeAnalysis] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_OSRLiveRangeAnalysis::create, OMR::osrLiveRangeAnalysis);
   _opts[OMR::tacticalGlobalRegisterAllocator] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_GlobalRegisterAllocator::create, OMR::tacticalGlobalRegisterAllocator);
   _opts[OMR::liveRangeSplitter] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LiveRangeSplitter::create, OMR::liveRangeSplitter);
   _opts[OMR::loopSpecializer] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR_LoopSpecializer::create, OMR::loopSpecializer);
   _opts[OMR::recognizedCallTransformer] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::RecognizedCallTransformer::create, OMR::recognizedCallTransformer);
   _opts[OMR::switchAnalyzer] =
       new (comp->allocator()) TR::OptimizationManager(self(), TR::SwitchAnalyzer::create, OMR::switchAnalyzer);

   // NOTE: Please add new OMR optimizations here!

   // initialize OMR optimization groups

   _opts[OMR::globalDeadStoreGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::globalDeadStoreGroup, globalDeadStoreOpts);
   _opts[OMR::loopCanonicalizationGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::loopCanonicalizationGroup, loopCanonicalizationOpts);
   _opts[OMR::loopVersionerGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::loopVersionerGroup, loopVersionerOpts);
   _opts[OMR::lastLoopVersionerGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::lastLoopVersionerGroup, lastLoopVersionerOpts);
   _opts[OMR::methodHandleInvokeInliningGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::methodHandleInvokeInliningGroup, methodHandleInvokeInliningOpts);
   _opts[OMR::earlyGlobalGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::earlyGlobalGroup, earlyGlobalOpts);
   _opts[OMR::earlyLocalGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::earlyLocalGroup, earlyLocalOpts);
   _opts[OMR::stripMiningGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::stripMiningGroup, stripMiningOpts);
   _opts[OMR::arrayPrivatizationGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::arrayPrivatizationGroup, arrayPrivatizationOpts);
   _opts[OMR::veryCheapGlobalValuePropagationGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::veryCheapGlobalValuePropagationGroup, veryCheapGlobalValuePropagationOpts);
   _opts[OMR::eachExpensiveGlobalValuePropagationGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::eachExpensiveGlobalValuePropagationGroup, eachExpensiveGlobalValuePropagationOpts);
   _opts[OMR::veryExpensiveGlobalValuePropagationGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::veryExpensiveGlobalValuePropagationGroup, veryExpensiveGlobalValuePropagationOpts);
   _opts[OMR::loopSpecializerGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::loopSpecializerGroup, loopSpecializerOpts);
   _opts[OMR::lateLocalGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::lateLocalGroup, lateLocalOpts);
   _opts[OMR::eachLocalAnalysisPassGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::eachLocalAnalysisPassGroup, eachLocalAnalysisPassOpts);
   _opts[OMR::tacticalGlobalRegisterAllocatorGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::tacticalGlobalRegisterAllocatorGroup, tacticalGlobalRegisterAllocatorOpts);
   _opts[OMR::partialRedundancyEliminationGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::partialRedundancyEliminationGroup, partialRedundancyEliminationOpts);
   _opts[OMR::reorderArrayExprGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::reorderArrayExprGroup, reorderArrayIndexOpts);
   _opts[OMR::blockManipulationGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::blockManipulationGroup, blockManipulationOpts);
   _opts[OMR::localValuePropagationGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::localValuePropagationGroup, localValuePropagationOpts);
   _opts[OMR::finalGlobalGroup] =
       new (comp->allocator()) TR::OptimizationManager(self(), NULL, OMR::finalGlobalGroup, finalGlobalOpts);

   // NOTE: Please add new OMR optimization groups here!
#endif
}

// Note: optimizer_name array needs to match Optimizations enum defined
// in compiler/optimizer/Optimizations.hpp
static const char *optimizer_name[] =
    {
#define OPTIMIZATION(name) #name,
#define OPTIMIZATION_ENUM_ONLY(entry) "****",
#include "optimizer/Optimizations.enum"
        OPTIMIZATION_ENUM_ONLY(numOpts)
#include "optimizer/OptimizationGroups.enum"
            OPTIMIZATION_ENUM_ONLY(numGroups)
#undef OPTIMIZATION
#undef OPTIMIZATION_ENUM_ONLY
};

const char *
OMR::Optimizer::getOptimizationName(OMR::Optimizations opt)
{
   return ::optimizer_name[opt];
}

bool OMR::Optimizer::isEnabled(OMR::Optimizations i)
{
   if (_opts[i] != NULL)
      return _opts[i]->enabled();
   return false;
}

TR_Debug *OMR::Optimizer::getDebug()
{
   return _compilation->getDebug();
}

void OMR::Optimizer::setCachedExtendedBBInfoValid(bool b)
{
   TR_ASSERT(!comp()->isPeekingMethod(), "ERROR: Should not modify _cachedExtendedBBInfoValid while peeking");
   _cachedExtendedBBInfoValid = b;
}

TR_UseDefInfo *OMR::Optimizer::setUseDefInfo(TR_UseDefInfo *u)
{
   if (_useDefInfo != NULL)
   {
      dumpOptDetails(comp(), "     (Invalidating use/def info)\n");
      delete _useDefInfo;
   }
   return (_useDefInfo = u);
}

TR_ValueNumberInfo *OMR::Optimizer::setValueNumberInfo(TR_ValueNumberInfo *v)
{
   if (_valueNumberInfo && !v)
      dumpOptDetails(comp(), "     (Invalidating value number info)\n");

   if (_valueNumberInfo)
      delete _valueNumberInfo;
   return (_valueNumberInfo = v);
}

TR_UseDefInfo *OMR::Optimizer::createUseDefInfo(TR::Compilation *comp,
                                                bool requiresGlobals, bool prefersGlobals, bool loadsShouldBeDefs, bool cannotOmitTrivialDefs,
                                                bool conversionRegsOnly, bool doCompletion)
{
   return new (comp->allocator()) TR_UseDefInfo(comp, comp->getFlowGraph(), self(), requiresGlobals, prefersGlobals, loadsShouldBeDefs,
                                                cannotOmitTrivialDefs, conversionRegsOnly, doCompletion, getCallsAsUses());
}

TR_ValueNumberInfo *OMR::Optimizer::createValueNumberInfo(bool requiresGlobals, bool preferGlobals, bool noUseDefInfo)
{
   LexicalTimer t("global value numbering (for globals definitely)", comp()->phaseTimer());
   TR::LexicalMemProfiler mp("global value numbering (for globals definitely)", comp()->phaseMemProfiler());

   TR_ValueNumberInfo *valueNumberInfo = NULL;
   switch (_vnInfoType)
   {
   case PrePartitionVN:
      valueNumberInfo = new (comp()->allocator()) TR_ValueNumberInfo(comp(), self(), requiresGlobals, preferGlobals, noUseDefInfo);
      break;
   case HashVN:
      valueNumberInfo = new (comp()->allocator()) TR_HashValueNumberInfo(comp(), self(), requiresGlobals, preferGlobals, noUseDefInfo);
      break;
   default:
      valueNumberInfo = new (comp()->allocator()) TR_ValueNumberInfo(comp(), self(), requiresGlobals, preferGlobals, noUseDefInfo);
      break;
   };

   TR_ASSERT(valueNumberInfo != NULL, "Failed to create ValueNumber Information");
   return valueNumberInfo;
}

void OMR::Optimizer::optimize()
{
   TR::Compilation::CompilationPhaseScope mainCompilationPhaseScope(comp());

   if (isIlGenOpt())
   {
      const OptimizationStrategy *opt = _strategy;
      while (opt->_num != endOpts)
      {
         TR::OptimizationManager *manager = getOptimization(opt->_num);
         TR_ASSERT(manager->getSupportsIlGenOptLevel(), "Optimization %s should support IlGen opt level", manager->name());
         opt++;
      }

      if (comp()->getOption(TR_TraceTrees) && (comp()->isOutermostMethod() || comp()->trace(inlining) || comp()->getOption(TR_DebugInliner)))
         comp()->dumpMethodTrees("Pre IlGenOpt Trees", getMethodSymbol());
   }

   LexicalTimer t("optimize", comp()->signature(), comp()->phaseTimer());
   TR::LexicalMemProfiler mp("optimize", comp()->signature(), comp()->phaseMemProfiler());
   TR::StackMemoryRegion stackMemoryRegion(*trMemory());

   // Sometimes the Compilation object needs to host more than one Optimizer
   // (over time).  This is because Symbol::genIL can be called, for example,
   // (indirectly) by addVeryRefinedCallAliasSets.  Under some circumstances,
   // genIL will instantiate a new Optimizer which must use the caller's
   // Compilation.  So, we need to push and pop the appropriate Optimizer.
   TR::Optimizer *stackedOptimizer = comp()->getOptimizer();
   _stackedOptimizer = (self() != stackedOptimizer);
   comp()->setOptimizer(self());

   if (comp()->getOption(TR_TraceOptDetails))
   {
      if (comp()->isOutermostMethod())
      {
         const char *hotnessString = comp()->getHotnessName(comp()->getMethodHotness());
         TR_ASSERT(hotnessString, "expected to have a hotness string");
         traceMsg(comp(), "<optimize\n"
                          "\tmethod=\"%s\"\n"
                          "\thotness=\"%s\">\n",
                  comp()->signature(), hotnessString);
      }
   }

   if (comp()->getOption(TR_TraceOpts))
   {
      if (comp()->isOutermostMethod())
      {
         const char *hotnessString = comp()->getHotnessName(comp()->getMethodHotness());
         TR_ASSERT(hotnessString, "expected to have a hotness string");
         traceMsg(comp(), "<strategy hotness=\"%s\">\n", hotnessString);
      }
   }

   int32_t firstOptIndex = comp()->getOptions()->getFirstOptIndex();
   int32_t lastOptIndex = comp()->getOptions()->getLastOptIndex();

   _firstDumpOptPhaseTrees = INT_MAX;
   _lastDumpOptPhaseTrees = INT_MAX;

   if (comp()->getOption(TR_TraceOptDetails))
      _firstDumpOptPhaseTrees = 0;

#ifdef DEBUG
   char *p;
   p = debug("dumpOptPhaseTrees");
   if (p)
   {
      _firstDumpOptPhaseTrees = 0;
      if (*p)
      {
         while (*p >= '0' && *p <= '9')
            _firstDumpOptPhaseTrees = _firstDumpOptPhaseTrees * 10 + *(p++) - '0';
         if (*(p++) == '-')
         {
            _lastDumpOptPhaseTrees = 0;
            while (*p >= '0' && *p <= '9')
               _lastDumpOptPhaseTrees = _lastDumpOptPhaseTrees * 10 + *(p++) - '0';
         }
         else
            _lastDumpOptPhaseTrees = _firstDumpOptPhaseTrees;
      }
   }

   static char *c3 = feGetEnv("TR_dumpGraphs");
   if (c3)
   {
      if (!debug("dumpGraphs"))
         addDebug("dumpGraphs");
      // Check if it is a number
      //
      if (*c3 >= '0' && *c3 <= '9')
         _dumpGraphsIndex = atoi(c3);
      else
         _dumpGraphsIndex = -1;
   }
#endif

   TR_SingleTimer myTimer;
   TR_FrontEnd *fe = comp()->fe();
   bool doTiming = comp()->getOption(TR_Timing);
   if (doTiming && comp()->getOutFile() != NULL)
   {
      myTimer.initialize("all optimizations", trMemory());
   }

   if (comp()->getOption(TR_Profile) && !comp()->isProfilingCompilation())
   {
      // These numbers are chosen to try to maximize the odds of finding bugs.
      // freq=2 means we'll switch to and from the profiling body often,
      // thus testing those transitions.
      // The low count value means we will try to recompile the method
      // fairly early, thus testing recomp.
      //
      self()->switchToProfiling(2, 30);
   }

   const OptimizationStrategy *opt = _strategy;
   while (opt->_num != endOpts)
   {
      int32_t actualCost = performOptimization(opt, firstOptIndex, lastOptIndex, doTiming);
      opt++;
      if (!isIlGenOpt() && comp()->getNodePool().removeDeadNodes())
      {
         setValueNumberInfo(NULL);
      }
   }

   if (comp()->getOption(TR_EnableDeterministicOrientedCompilation) &&
       comp()->isOutermostMethod() &&
       (comp()->getMethodHotness() > cold) &&
       (comp()->getMethodHotness() < scorching))
   {
      TR_Hotness nextHotness = checkMaxHotnessOfInlinedMethods(comp());
      if (nextHotness > comp()->getMethodHotness())
      {
         comp()->setNextOptLevel(nextHotness);
         comp()->failCompilation<TR::InsufficientlyAggressiveCompilation>("Method needs to be compiled at higher level");
      }
   }

   dumpPostOptTrees();

   if (comp()->getOption(TR_TraceOpts))
   {
      if (comp()->isOutermostMethod())
         traceMsg(comp(), "</strategy>\n");
   }

   if (comp()->getOption(TR_TraceOptDetails))
   {
      if (comp()->isOutermostMethod())
         traceMsg(comp(), "</optimize>\n");
   }

   comp()->setOptimizer(stackedOptimizer);
   _stackedOptimizer = false;
}

void OMR::Optimizer::dumpPostOptTrees()
{
   // do nothing for IlGen optimizer
   if (isIlGenOpt())
      return;

   TR::Method *method = comp()->getMethodSymbol()->getMethod();
   if ((debug("dumpPostLocalOptTrees") || comp()->getOption(TR_TraceTrees)))
      comp()->dumpMethodTrees("Post Optimization Trees");
}

void dumpName(TR::Optimizer *op, TR_FrontEnd *fe, TR::Compilation *comp, OMR::Optimizations optNum)
{
   static int level = 1;
   TR::OptimizationManager *manager = op->getOptimization(optNum);

   if (level > 6)
      return;

   if (optNum > endGroup && optNum < OMR::numGroups)
   {
      trfprintf(comp->getOutFile(), "%*s<%s>\n", level * 6, " ", manager->name());

      level++;

      const OptimizationStrategy *subGroup = ((TR::OptimizationManager *)manager)->groupOfOpts();

      while (subGroup->_num != endOpts && subGroup->_num != endGroup)
      {
         dumpName(op, fe, comp, subGroup->_num);
         subGroup++;
      }

      level--;

      trfprintf(comp->getOutFile(), "%*s</%s>", level * 6, " ", manager->name());
   }
   else if (optNum > endOpts && optNum < OMR::numOpts)
      trfprintf(comp->getOutFile(), "%*s%s", level * 6, " ", manager->name());
   else
      trfprintf(comp->getOutFile(), "%*s<%d>", level * 6, " ", optNum);

   trfprintf(comp->getOutFile(), "\n");
}

void OMR::Optimizer::dumpStrategy(const OptimizationStrategy *opt)
{
   TR_FrontEnd *fe = comp()->fe();

   trfprintf(comp()->getOutFile(), "endOpts:%d OMR::numOpts:%d endGroup:%d numGroups:%d\n", endOpts, OMR::numOpts, endGroup, OMR::numGroups);

   while (opt->_num != endOpts)
   {
      dumpName(self(), fe, comp(), opt->_num);
      opt++;
   }

   trfprintf(comp()->getOutFile(), "\n");
}

static bool hasMoreThanOneBlock(TR::Compilation *comp)
{
   return (comp->getStartBlock() && comp->getStartBlock()->getNextBlock());
}

static void breakForTesting(int index)
{
   static char *optimizerBreakLocationStr = feGetEnv("TR_optimizerBreakLocation");
   if (optimizerBreakLocationStr)
   {
      static int optimizerBreakLocation = atoi(optimizerBreakLocationStr);
      static char *optimizerBreakSkipCountStr = feGetEnv("TR_optimizerBreakSkipCount");
      static int optimizerBreakSkipCount = optimizerBreakSkipCountStr ? atoi(optimizerBreakSkipCountStr) : 0;
      if (index == optimizerBreakLocation)
      {
         if (optimizerBreakSkipCount == 0)
            TR::Compiler->debug.breakPoint();
         else
            --optimizerBreakSkipCount;
      }
   }
}

// extern std::map<string, Ptg> parsePTG(string staticFileName);

std::string getFormattedCurrentMethodName(TR::ResolvedMethodSymbol *methodSymbol)
{

   int methodNameLength = methodSymbol->getMethod()->nameLength();
   string methodName = methodSymbol->getMethod()->nameChars();

   return methodName.substr(0, methodNameLength);
}

std::string getFormattedCurrentClassName(TR::ResolvedMethodSymbol *methodSymbol)
{

   int classNameLength = methodSymbol->getMethod()->classNameLength();
   string className = methodSymbol->getMethod()->classNameChars();

   return className.substr(0, classNameLength);
}

std::string getLoopInvariantStaticFileName(std::string className, std::string methodName)
{
   string loopInvariantStaticFileName = "loop-invariants-";
   loopInvariantStaticFileName += className;
   loopInvariantStaticFileName += ".";
   loopInvariantStaticFileName += methodName;
   loopInvariantStaticFileName += ".txt";

   std::replace(loopInvariantStaticFileName.begin(), loopInvariantStaticFileName.end(), '/', '-');

   return loopInvariantStaticFileName;
}

std::string getCallSiteInvariantStaticFileName(std::string className, std::string methodName)
{

   string callSiteInvariantStaticFileName = "callsite-invariants-";
   callSiteInvariantStaticFileName += className;
   callSiteInvariantStaticFileName += ".";
   callSiteInvariantStaticFileName += methodName;
   callSiteInvariantStaticFileName += ".txt";

   std::replace(callSiteInvariantStaticFileName.begin(), callSiteInvariantStaticFileName.end(), '/', '-');
   return callSiteInvariantStaticFileName;
}

// //check if ptg1 subsumes ptg2
// bool checkPTGSubsumes(Ptg ptg1, Ptg ptg2)
// {
//    bool subsumes = true;

//    for (std::map<int, std::set<std::string> >::iterator it = ptg2.varsMap.begin(); it != ptg2.varsMap.end(); ++it)
//    {
//       int key = it->first;
//       std::set<std::string> set = it->second;

//       if (ptg1.varsMap.find(key) != ptg1.varsMap.end())
//       {
//          //the key is present, now verify the subsumes relation for the set as well
//          std::set<std::string> ptg1set = ptg1.varsMap.find(key)->second;

//          for (std::set<std::string>::iterator s = set.begin(); s != set.end(); ++s)
//          {
//             if (ptg1set.find(*s) == ptg1set.end())
//             {
//                subsumes = false;
//                return subsumes;
//             }
//          }
//       }
//    }

//    for (std::map<std::string, std::set<std::string>>::iterator it = ptg2.fieldsMap.begin(); it != ptg2.fieldsMap.end(); ++it)
//    {
//       std::string key = it->first;
//       std::set<std::string> set = it->second;

//       //ptg1 must contain this key, if not - fail validation
//       if (ptg1.fieldsMap.find(key) == ptg1.fieldsMap.end())
//       {
// //#ifdef RUNTIME_PTG_DEBUG
//          std::cout << "invariance verification failed" << endl;
// //#endif
//          subsumes = false;
//          return subsumes;
//       }
//       else
//       {
//          //the key is present, now verify the subsumes relation for the set as well
//          std::set<std::string> ptg1set = ptg1.fieldsMap.find(key)->second;

//          for (std::set<std::string>::iterator s = set.begin(); s != set.end(); ++s)
//          {
//             if (ptg1set.find(*s) == ptg1set.end())
//             {
//                subsumes = false;
//                return subsumes;
//             }
//          }
//       }
//    }

//    return subsumes;

// }
// Ptg meetPTGs(Ptg ptg1, Ptg ptg2)
// {
//    Ptg res;
//    std::map<int, std::set<std::string>> varsMap;
//    std::map<std::string, std::set<std::string>> fieldsMap;

//    //merge the varsMap first
//    //1. add all the Roots from ptg1
//    auto vIt1 = ptg1.varsMap.begin();
//    while (vIt1 != ptg1.varsMap.end())
//    {
//       varsMap.insert(std::pair<int, std::set<std::string>>(vIt1->first, vIt1->second));
//       vIt1++;
//    }
//    //2. now merge in the Roots from ptg2

//    auto vIt2 = ptg2.varsMap.begin();
//    while (vIt2 != ptg2.varsMap.end())
//    {
//       if (varsMap.find(vIt2->first) == varsMap.end())
//       {
//          varsMap.insert(std::pair<int, std::set<std::string>>(vIt2->first, vIt2->second));
//       }
//       //obviously there's a chance that the var is already accounted for by ptg1's Roots, in this case
//       //merge the Roots set for that var
//       else
//       {
//          auto ptg1Set = varsMap.find(vIt2->first)->second;
//          auto ptg2Set = vIt2->second;
//          std::set<std::string> tempSet;
//          std::merge(ptg1Set.begin(), ptg1Set.end(), ptg2Set.begin(), ptg2Set.end(), std::inserter(tempSet, tempSet.begin()));

//          varsMap.erase(vIt2->first);
//          varsMap.insert(std::pair<int, std::set<std::string>>(vIt2->first, tempSet));
//       }

//       vIt2++;
//    }

//    //then the fieldsMap
//    //1. add all the Heap sets from ptg1
//    auto fIt1 = ptg1.fieldsMap.begin();
//    while (fIt1 != ptg1.fieldsMap.end())
//    {
//       fieldsMap.insert(std::pair<std::string, std::set<std::string>>(fIt1->first, fIt1->second));
//       fIt1++;
//    }
//    //2. now merge in the Heap sets from ptg2

//    auto fIt2 = ptg2.fieldsMap.begin();
//    while (fIt2 != ptg2.fieldsMap.end())
//    {
//       if (fieldsMap.find(fIt2->first) == fieldsMap.end())
//       {
//          fieldsMap.insert(std::pair<string, std::set<std::string>>(fIt2->first, fIt2->second));
//       }
//       else
//       {
//          auto ptg1Set = fieldsMap.find(fIt2->first)->second;
//          auto ptg2Set = fIt2->second;
//          std::set<std::string> tempSet;
//          std::merge(ptg1Set.begin(), ptg1Set.end(), ptg2Set.begin(), ptg2Set.end(), std::inserter(tempSet, tempSet.begin()));

//          fieldsMap.erase(fIt2->first);
//          fieldsMap.insert(std::pair<std::string, std::set<std::string>>(fIt2->first, tempSet));

//       }

//       fIt2++;
//    }

//    res.varsMap = varsMap;
//    res.fieldsMap = fieldsMap;

// #ifdef RUNTIME_PTG_DEBUG
//    cout << "completed Meet operation, result: " << endl;
//    res.print();
// #endif

//    return res;
// }

// //returns the meet of the out-PTGs of all the predecessors of the requested block
// Ptg getPredecessorPTG(TR::Block *bl, std::map<int, Ptg> outPTGs)
// {
//    auto requestedBlockNumber = bl->getNumber();
// #ifdef RUNTIME_PTG_DEBUG
//    cout << "*computing the predecessor PTG for block " << requestedBlockNumber << endl;
// #endif

//    Ptg predMeetPTG;
//    for (TR::CFGEdgeList::iterator pred = bl->getPredecessors().begin(); pred != bl->getPredecessors().end(); ++pred)
//    {
//       TR::Block *predBlock = toBlock((*pred)->getFrom());
//       auto predBlockNumber = predBlock->getNumber();
// #ifdef RUNTIME_PTG_DEBUG
//       cout << "*checking predecessor block BB " << predBlockNumber << endl;
// #endif

//       if (outPTGs.find(predBlockNumber) != outPTGs.end())
//       {
//          Ptg outPTG = outPTGs.find(predBlockNumber)->second;
//          //Ptg temp;
//          predMeetPTG = meetPTGs(predMeetPTG, outPTG);
//       }
//    }

//    return predMeetPTG;
// }

// void processAllocation(TR::Node *node, Ptg basicBlockPtg, TR::Compilation *comp) {
//    cout << "entered processAllocation" << endl;
//    //the node here is really a firstchild

//    /*
//     * change this idiotic allocSymRef to the node number
//     * the newly created object can be uniquely identified by the node number, instead of the
//     * symref (which could be reused multiple times, especially in case of pending push temps)
//     */

//    std::string nodeNumber = node->getName(comp->getDebug());

//    int allocSymRef = node->getSymbolReference()->getReferenceNumber();

//    int bci = node->getByteCodeInfo().getByteCodeIndex();

//    if (basicBlockPtg.varsMap.find(allocSymRef) != basicBlockPtg.varsMap.end())
//    {
//       //there is already an entry against this bci
//       //std::vector<int> v = bbPTG.find(allocSymRef)->second;
//       std::set<std::string> s;
//       s.insert(to_string(bci));
//       basicBlockPtg.varsMap.erase(allocSymRef);
//       basicBlockPtg.varsMap.insert(std::pair<int, std::set<std::string>>(allocSymRef, s));
//    }
//    else
//    {
//       std::set<std::string> s;
//       s.insert(to_string(bci));
//       basicBlockPtg.varsMap.insert(std::pair<int, std::set<std::string>>(allocSymRef, s));
//    }
//    cout << "exiting processAllocation" << endl;
// }

// recursively goes down the children of the node and returns the first "useful" child, else null if no useful children
TR::Node *getUsefulNode(TR::Node *node)
{
   IFDIAGPRINT << "checking node n" << node->getGlobalIndex() << "n for a useful node" << endl;

   TR::Node *ret = NULL;
   if (node == NULL)
      return ret;
   else
   {
      TR::ILOpCodes opCode = node->getOpCodeValue();
      // if the opcodes is one of the following, we need to dig deeper
      if (opCode == TR::treetop || opCode == TR::ResolveAndNULLCHK || opCode == TR::ResolveCHK || opCode == TR::compressedRefs || opCode == TR::NULLCHK)
         return getUsefulNode(node->getFirstChild());

      else
      {
         // these are the interesting nodes
         if (opCode == TR::New ||
             opCode == TR::astore ||
             opCode == TR::astorei ||
             opCode == TR::Return ||
             opCode == TR::areturn ||
             opCode == TR::aload ||
             opCode == TR::aloadi ||
             opCode == TR::call ||
             opCode == TR::calli ||
             opCode == TR::acalli ||
             opCode == TR::acall ||
             opCode == TR::calli ||
             opCode == TR::awrtbari ||
             opCode == TR::ardbari ||
             node->getOpCode().isCall())
         {
            IFDIAGPRINT << "found useful node at n" << node->getGlobalIndex() << "n" << endl;
            ret = node;
         }
         else
         {
            // TODO: Shashin: insert an assert failure here
         }
      }
   }

   return ret;
}

//  void verifyStaticMethodInfo(std::string className, std::string methodName, TR::CFG *cfg, TR::Compilation *comp)
// {
//    cout << "running verifyStaticMethodInfo for " << className << ":" << methodName << endl;
//    auto staticLoopInvariantFileName = getLoopInvariantStaticFileName(className, methodName);
//    //read in the static dump for loop invariants
//    std::map<string, Ptg> staticLoopInvariants;
//    //staticLoopInvariants = parsePTG(staticLoopInvariantFileName);

//    //if (trace())
//    {
//       traceMsg(comp, "Attempted to read static loop invariant file \"%s\"\n", staticLoopInvariantFileName.c_str());
//    }

//    //maintain the PTG in a before-after format. Essentially, the flow function should transform the "before" to the "after"
//    std::map<int, Ptg> ptgsBefore;
//    std::map<int, Ptg> ptgsAfter;
//    //a convenience map to track the out-ptgs of each basic block. Makes propagation easier
//    std::map<int, Ptg> outPTGs;
//    std::map<int, Ptg> stackSlotMappedInvariantPTGs;

//    //fetch the CFG's start
//    TR::Block *startBlock = cfg->getStart()->asBlock();

//    //push the start block of the CFG to the queue
//    std::queue<TR::Block *> successorQueue;
//    successorQueue.push(startBlock);
//    //keep track of which basic blocks have been visited
//    std::set<int> visited;

//    Ptg latestPTG;

//    //a simple BFS traversal of the CFG, starting from the root
//    while (!successorQueue.empty())
//    {

//       TR::Block *currentBlock = successorQueue.front();
//       successorQueue.pop();

//       int currentBlockNumber = currentBlock->getNumber();

// #ifdef RUNTIME_PTG_DEBUG
//       cout << "now checking BB " << currentBlockNumber << endl;
// #endif

//       if (visited.find(currentBlockNumber) != visited.end())
//       {
// //this block has already been processed
// #ifdef RUNTIME_PTG_DEBUG
//          cout << "BB " << currentBlockNumber << " already visited" << endl;
// #endif
//          continue;
//       }
//       else
//       {
//          visited.insert(currentBlockNumber);
//       }

//       auto predecessorMeetPTG = getPredecessorPTG(currentBlock, outPTGs);

//       cout << "completed getPredecessorPTG" << endl;

//       //TODO - this needs to be the "before" Ptg of the first bci in this block
//       latestPTG = predecessorMeetPTG;

//       TR::TreeTop *tt = currentBlock->getEntry();
//       if(tt == NULL) return;
//       for (; tt; tt = tt->getNextRealTreeTop())
//       {

//          //a "local" PTG for the current basic block
//          //note that we don't need to worry about flow of control here, since it is linear.
//          Ptg currentBBPTG = latestPTG;

//          //TODO : can we not make this the bounds of the loop itself?
//          TR::Node *node = tt->getNode();
//          if (node->getOpCodeValue() == TR::BBStart)
//             continue;
//          else if (node->getOpCodeValue() == TR::BBEnd)
//             break;

//          int bci = node->getByteCodeInfo().getByteCodeIndex();

//          //check if there is a static PTG available for this bci
//          if (staticLoopInvariants.find(to_string(bci)) != staticLoopInvariants.end())
//          {
// #ifdef RUNTIME_PTG_DEBUG
//             cout << "found static loop invariant at bci " << bci << endl;
// #endif

//             auto staticLoopInvariantPTG = staticLoopInvariants.find(to_string(bci))->second;
//             staticLoopInvariantPTG.print();

//             //there is a static loop invariant PTG available at this bci
//             //so proceed to map it to the running PTG

//             //1. traverse the symref table to match the stack slots with their respective SymRefs.

//             TR::SymbolReferenceTable *tab = comp->getSymRefTab();
//             std::map<int, int> stackSlotToSymRefMap;
//             for (int i = tab->getIndexOfFirstSymRef(); i < tab->getNumSymRefs(); i++)
//             {

//                TR::SymbolReference *sym = tab->getSymRef(i);

//                //if (sym != NULL && !sym->isThisPointer() && sym->getSymbol()->getType().isAddress() && sym->getCPIndex() > 0) {
//                if (sym != NULL && sym->getSymbol()->getType().isAddress() && sym->getSymbol()->isAuto())
//                {
//                   stackSlotToSymRefMap.insert(std::pair<int, int>(sym->getCPIndex(), sym->getReferenceNumber()));
//                }
//             }

// #ifdef RUNTIME_PTG_DEBUG
//             auto it = stackSlotToSymRefMap.begin();
//             while (it != stackSlotToSymRefMap.end())
//             {
//                cout << it->first << "->" << it->second << endl;
//                it++;
//             }
// #endif

// //2. now map the static PTG to the running PTG
// #ifdef RUNTIME_PTG_DEBUG
//             cout << "**before mapping static to runtime\n";
//             currentBBPTG.print();
// #endif

//             auto vIt = staticLoopInvariantPTG.varsMap.begin();
//             while (vIt != staticLoopInvariantPTG.varsMap.end())
//             {
//                auto stackSlot = vIt->first;
//                auto symRef = stackSlotToSymRefMap.find(stackSlot)->second;
//                auto bciVals = vIt->second;

//                currentBBPTG.varsMap.erase(symRef);
//                currentBBPTG.varsMap.insert(std::pair<int, std::set<std::string>>(symRef, bciVals));

//                vIt++;
//             }

//             auto fIt = staticLoopInvariantPTG.fieldsMap.begin();
//             while (fIt != staticLoopInvariantPTG.fieldsMap.end())
//             {
//                auto fieldKey = fIt->first;
//                auto bciVals = fIt->second;

//                currentBBPTG.fieldsMap.erase(fieldKey);
//                currentBBPTG.fieldsMap.insert(std::pair<std::string, std::set<std::string>>(fieldKey, bciVals));

//                fIt++;
//             }

// #ifdef RUNTIME_PTG_DEBUG
//             cout << "**after mapping static to runtime\n";
//             currentBBPTG.print();
// #endif
//             stackSlotMappedInvariantPTGs.insert(std::pair<int, Ptg>(bci, currentBBPTG));
//             //latestPTG = currentBBPTG;

//          } //end mapping of static invariants to stack slots

//          TR::Node * usefulNode = getUsefulNode(node);
//          if(usefulNode != NULL){
//             TR::ILOpCode opCode = usefulNode->getOpCode();
//             if(opCode.isNew()) {
//                processAllocation(node, currentBBPTG, comp);
//             } else if (opCode.isStore()) {
//                //processStore();
//             } else if (opCode.isLoad()) {
//                //processLoad();
//             } else if (opCode.isCall()) {
//                //processCall();
//             } else if (opCode.isReturn()) {
//                //processReturn;
//             }
//          }

// #ifdef RUNTIME_PTG_DEBUG
//          cout << "Current BB Ptg after processing node " << node->getNumber() << endl;
//          currentBBPTG.print();
// #endif

//          ptgsAfter.insert(std::pair<int, Ptg>(bci, currentBBPTG));
//          latestPTG = currentBBPTG;

//       } //end for-loop iterating over treetops for the current BB

//       /*
//       * if we are here, we have reached the BBEnd of the current basic block, in this case we want to perform two activities:
//       * 1. add the successors of this block to the successor queue
//       * 2. associate the latest PTG as the out-PTG of the current block
//       */

// #ifdef RUNTIME_PTG_DEBUG
//       cout << "Visited CFG blocks : ";
//       for (auto it = visited.begin(); it != visited.end(); ++it)
//       {
//          cout << " " << *it;
//       }
//       cout << endl;
// #endif

//       for (TR::CFGEdgeList::iterator succ = currentBlock->getSuccessors().begin(); succ != currentBlock->getSuccessors().end(); ++succ)
//       {
//          TR::Block *bl = toBlock((*succ)->getTo());
// #ifdef RUNTIME_PTG_DEBUG
//          cout << "checking successor block " << bl->getNumber() << endl;
// #endif
//          if (visited.find(bl->getNumber()) == visited.end())
//          {
// #ifdef RUNTIME_PTG_DEBUG
//             cout << "**checking successor block " << bl->getNumber() << " into queue" << endl;
// #endif
//             successorQueue.push(bl);
//          } else {

//             //this successor block has been visited before, check for invariance.
//             auto predPTG = getPredecessorPTG(bl, outPTGs);
//             //perform a meet between predecessor PTG and the latest PTG
//             auto meetPTG = meetPTGs(predPTG, latestPTG);
//             //this predecessor-meet-PTG should match the block's current outgoing PTG
//             //TODO : why is getFirstRealTreeTop throwing an exception here?
//             int bci = -1;
//             //TR_ASSERT_FATAL(true, "true hardcoded assert");
//             //TR_ASSERT_FATAL(bl->getEntry() && bl->getEntry()->getNextTreeTop(), "entry treetops are null");

//             cout << "attempting getFirstRealTreeTop, BB " << bl->getNumber() << endl;
//             if(bl->getEntry() && bl->getEntry()->getNextTreeTop()) {
//                bci = bl->getFirstRealTreeTop()->getNode()->getByteCodeInfo().getByteCodeIndex();
//             }

//             cout << "finished attempt to getFirstRealTreeTop" << endl;
//             auto bciStr = to_string(bci);
//             Ptg ptgToCheckSubsumes;
//             //Ptg outPTG = outPTGs.find(bl->getNumber())->second;

//             // if (staticLoopInvariants.find(bci) != staticLoopInvariants.end())
//             // {
//             //    ptgToCheckSubsumes = staticLoopInvariants.find(bci)->second;
//             // }
//             if (stackSlotMappedInvariantPTGs.find(bci) != stackSlotMappedInvariantPTGs.end())
//             {
//                ptgToCheckSubsumes = stackSlotMappedInvariantPTGs.find(bci)->second;
//                cout << "completed ptgToCheckSubsumes computation" << endl;
//             }

//             else
//             {
//                ptgToCheckSubsumes = predPTG;
//             }

//             if (!checkPTGSubsumes(ptgToCheckSubsumes, meetPTG))
//             //if(!checkPTGSubsumes2(meetPTG, outPTG))
//             {
//                //if (trace())
//                //{
//                   traceMsg(comp, "Runtime Verification Results: verification failed, invariant does not hold for block %d\n", bl->getNumber());
//                   traceMsg(comp, "************************************************************************************************\n");
//                //}
//                return;
//             }
//             else
//             {
//                //if (trace())
//                //{
//                   traceMsg(comp, "Runtime Verification Results: success!\n");
//                   traceMsg(comp, "************************************************************************************************\n");
//                //}
//             }
//             cout << "completed checkPTGSubsumes invocation" << endl;
//          }

//       } // end for-loop iterating over BB successors
//       cout << "inserting to outptgs" << endl;
//       outPTGs.insert(std::pair<int, Ptg>(currentBlockNumber, latestPTG));
//       cout << "completed inserting to outptgs" << endl;

//     } // end while- iteration over successor BBs queue

// /*
//    //if(trace()){
//       traceMsg(comp, "Generated Points-To Maps\n");
//       for(auto it = ptgsAfter.begin(); it != ptgsAfter.end(); ++it) {
//          traceMsg(comp, "BCI %d:\n", it->first);
//          traceMsg(comp, it->second.toString().c_str());
//       }
//       traceMsg(comp, "************************************************************************************************\n");
//    //}

// #ifdef RUNTIME_PTG_DEBUG
//    for (auto it = ptgsAfter.begin(); it != ptgsAfter.end(); ++it)
//    {
//       cout << "BCI " << it->first << ":" << endl
//            << "\t";
//       it->second.print();
//       cout << endl;
//    }
// #endif
// */

// }

void performRuntimeVerification2(TR::Compilation *comp)
{
   std::string currentClassName = getFormattedCurrentClassName(comp->getMethodSymbol());
   std::string currentMethodName = getFormattedCurrentMethodName(comp->getMethodSymbol());
   std::string sig = currentClassName + "." + currentMethodName;
   if (_runtimeVerifiedMethods.find(sig) != _runtimeVerifiedMethods.end())
      // this method has already been analyzed
      return;
   else
      _runtimeVerifiedMethods.insert(sig);

   // std::cout << "running performRuntimeVerification for " << currentMethodName << endl;

   TR::CFG *cfg = comp->getFlowGraph();

   // traceMsg(comp, "CFG for callee method %s\n", currentMethodName);
   comp->dumpFlowGraph(cfg);
   comp->dumpMethodTrees("from OMR::Optimizer::performRuntimeVerification");

   // recursively invoke the verification algorithm (calls itself at each callsite to analyze the called method)
   // verifyStaticMethodInfo(currentClassName, currentMethodName, cfg, comp);
}

void printRuntimeVerifierDiagnostic(string message)
{
   if (_runtimeVerifierDiagnostics)
      cout << message;
}

// Sets all method parameters to BOT, including the this parameter
int bottomizeParameters(TR::ResolvedMethodSymbol *methodSymbol, PointsToGraph *in)
{
   ListIterator<TR::ParameterSymbol> paramIterator(&(methodSymbol->getParameterList()));
   TR::SymbolReference *symRef;
   for (TR::ParameterSymbol *paramCursor = paramIterator.getFirst(); paramCursor != NULL; paramCursor = paramIterator.getNext())
   {
      // param at getSlot == 0 is the this-pointer
      symRef = methodSymbol->getParmSymRef(paramCursor->getSlot());
      int32_t symRefNumber = symRef->getReferenceNumber();

      if (_runtimeVerifierDiagnostics)
         cout << "the symref number corresponding to param " << paramCursor->getSlot() << " is " << symRefNumber << endl;

      in->assignBot(symRefNumber);
   }

   return 0;
}

// Maps the paremeter symrefs to their appropriate values in the inFlow
int mapParametersIn(TR::ResolvedMethodSymbol *methodSymbol, PointsToGraph *in)
{

   ListIterator<TR::ParameterSymbol> paramIterator(&(methodSymbol->getParameterList()));
   TR::ParameterSymbol *paramCursor = paramIterator.getFirst();
   TR::SymbolReference *symRef;
   bool isVirtualOrSpecial = false;
   if (methodSymbol->isVirtual() || methodSymbol->isSpecial())
   {
      // map off the this parm first
      //      symRef = methodSymbol->getParmSymRef(0);
      //      int32_t symRefNumber = symRef->getReferenceNumber();
      //
      //      set <Entry> thisPointees = in->getArgPointsToSet(0);
      //      in->assign(symRefNumber, thisPointees);
      //
      //      argIndex++;
      //      paramCursor = paramIterator.getNext();

      isVirtualOrSpecial = true;
   }

   for (; paramCursor != NULL; paramCursor = paramIterator.getNext())
   {
      // param at getSlot == 0 is the this-pointer
      int paramSlot = paramCursor->getSlot();

      int argIndex = paramSlot;
      if (!isVirtualOrSpecial)
         argIndex++;

      symRef = methodSymbol->getParmSymRef(paramSlot);
      if (symRef->getSymbol()->getType().isAddress())
      {

         int32_t symRefNumber = symRef->getReferenceNumber();

         if (_runtimeVerifierDiagnostics)
            cout << "the symref number corresponding to param " << paramCursor->getSlot() << " is " << symRefNumber << " will be mapped to param " << argIndex << endl;

         set<Entry> argsPointsTo = in->getArgPointsToSet(argIndex);
         in->assign(symRefNumber, argsPointsTo);
         argIndex++;
      }
   }

   return 0;
}

void mapParametersOut(TR::ResolvedMethodSymbol *methodSymbol, TR::Node *callNode, set<Entry> &evaluatedNodeValue, PointsToGraph *in)
{

   ListIterator<TR::ParameterSymbol> paramIterator(&(methodSymbol->getParameterList()));
   TR::SymbolReference *symRef;
   // invokespecial (private instance calls, constructors, etc - labeled 'special' in the IL tree) and invokevirtual (public, package private, defaults,)
   //  pass the this-parm. However the trees are slightly different
   int argIndex = 0;
   if (methodSymbol->isVirtual())
   {
      // the first arg for a virtual call is the load of the VFT, we shall skip that
      TR::Node *thisParmNode = callNode->getSecondChild();
   }
   else if (methodSymbol->isSpecial())
   {
      // no VFTs here, pick off the args directly
      argIndex++;
   }
   else if (methodSymbol->isStatic())
   {
      // no VFT or this-param, pick off the args directly
      argIndex++;
   }
   else if (methodSymbol->isInterface())
   {
      // invokeinterface do not seem to get resolved - what do we do ??
      argIndex++;
   }

   //   for (TR::ParameterSymbol *paramCursor = paramIterator.getFirst(); paramCursor != NULL; paramCursor = paramIterator.getNext())
   //   {
   //      // param at getSlot == 0 is the this-pointer
   //      int paramSlot = paramCursor->getSlot();
   //      symRef = methodSymbol->getParmSymRef(paramSlot);
   //      int32_t symRefNumber = symRef->getReferenceNumber();
   //
   //      if (_runtimeVerifierDiagnostics)
   //         cout << "the symref number corresponding to param " << paramCursor->getSlot() << " is " << symRefNumber << endl;
   //
   //      set <Entry> argsPointsTo = in->getArgPointsToSet(paramSlot);
   //      in->assign(symRefNumber, argsPointsTo);
   //   }
}
PointsToGraph *meet(PointsToGraph *a, PointsToGraph *b)
{
   PointsToGraph *res = new PointsToGraph();
   res->ptgUnion(a, b);
   return res;
}
/*
 * Returns the meet of the out-PTGs of all predecessor blocks
 */
PointsToGraph *getPredecessorMeet(TR::Block *bl, std::map<TR::Block *, PointsToGraph *> basicBlockOuts)
{
   PointsToGraph *predecessorMeet = new PointsToGraph();

   for (TR::CFGEdgeList::iterator pred = bl->getPredecessors().begin(); pred != bl->getPredecessors().end(); ++pred)
   {
      TR::Block *predBlock = toBlock((*pred)->getFrom());
      if (basicBlockOuts.find(predBlock) != basicBlockOuts.end())
      {
         PointsToGraph *predOut = basicBlockOuts[predBlock];

         predecessorMeet = meet(predecessorMeet, predOut);
      }
   }

   return predecessorMeet;
}

/*
 * applies the points-to analysis flow function for an allocation statement
 * - we need to store away the bci at which the object is being created, against the respective node
 */
int processAllocation(PointsToGraph *in, TR::Node *node, std::map<TR::Node *, vector<int>> &evaluatedNodeValues, int visitCount)
{
   int ret = 0;
   int nodeGlobalIndex = node->getGlobalIndex();
   if (_runtimeVerifierDiagnostics)
      cout << "the allocation is at node " << nodeGlobalIndex << endl;

   // TR_ASSERT_FATAL(evaluatedNodeValues.find(node) == evaluatedNodeValues.end(), "we assumed that allocation nodes are never re-processed!");

   if (node->getVisitCount() < visitCount)
   {
      node->setVisitCount(visitCount);

      int allocationBCI = node->getByteCodeIndex();
      vector<int> evaluatedNodeValue;
      evaluatedNodeValue.push_back(allocationBCI);
      evaluatedNodeValues.insert(std::pair<TR::Node *, vector<int>>(node, evaluatedNodeValue));

      if (_runtimeVerifierDiagnostics)
         cout << "evaluated an allocation node n" << nodeGlobalIndex << "n, object @bci " << allocationBCI << endl;
   }
   else
   {
      // we may need to error here.
   }

   return ret;
}

int processStore(PointsToGraph *in, TR::Node *node, std::map<TR::Node *, vector<int>> &evaluatedNodeValues, int visitCount)
{

   IFDIAGPRINT << "the store node has " << node->getNumChildren() << " child nodes, its sym ref is "
               << node->getSymbolReference()->getReferenceNumber() << endl;
   // TODO: assert(node->getNumChildren() == 1);
   TR_ASSERT_FATAL(node->getNumChildren() == 1, "we assumed that astore always has a single child");

   // look at the child of the astore - if it is not evaluated, evaluate it
   TR::Node *childNode = node->getFirstChild();
   TR_ASSERT_FATAL(childNode != NULL, "we assumed that the astore's child is non-null");
   int childNodeIndex = childNode->getGlobalIndex();

   // if(evaluatedNodeValues.find(childNode) != evaluatedNodeValues.end()) {
   //    //the node's been evaluated before
   // } else {
   //    //the node's not been evaluated, do it now
   // }

   if (childNode->getVisitCount() < visitCount)
   {
      if (_runtimeVerifierDiagnostics)
         cout << childNode->getGlobalIndex() << " visted first time" << endl;
      // think of evaluating the node here
   }
   else
   {
      cout << childNode->getGlobalIndex() << " visited already" << endl;
      vector<int> evaluatedNodeValue = evaluatedNodeValues.find(childNode)->second;
      // in->assign(node->getSymbolReference()->getReferenceNumber(), evaluatedNodeValue);
      if (_runtimeVerifierDiagnostics)
         in->print();
   }

   return 0;
}

#define THISVAR 0
#define RETURNVAR -77

Entry evaluateAllocate(TR::Node *node, int methodIndex)
{
   if (_runtimeVerifierDiagnostics)
      cout << "evaluated an allocation node at n" << node->getGlobalIndex() << "n" << endl;
   int allocationBCI = node->getByteCodeIndex();
   int callerIndex = methodIndex;

   Entry entry;
   entry.bci = allocationBCI;
   entry.caller = methodIndex;
   entry.type = Reference;

   return entry;
}

PointsToGraph *verifyStaticMethodInfo(int visitCount, TR::Compilation *comp, TR::ResolvedMethodSymbol *methodSymbol,
                                      std::string className, std::string methodName, PointsToGraph *inFlow, bool isInvokedByJITC);

int getOrInsertMethodIndex(string methodSignature)
{
   if (_methodIndices.find(methodSignature) != _methodIndices.end())
   {
      return _methodIndices[methodSignature];
   }
   else
   {
      int index = _methodIndices.size();
      _methodIndices[methodSignature] = ++index;
      return index;
   }
}

void mapParameters(PointsToGraph *pred, PointsToGraph *inFlow, TR::Node *callNode)
{
}

PointsToGraph *performRuntimePointsToAnalysis(PointsToGraph *inFlow, TR::ResolvedMethodSymbol *methodSymbol, int visitCount);
// recursively evaluates a node and returns its evaluated value. it may have a side effect of updating the points-to maps.
set<Entry> evaluateNode(PointsToGraph *in, TR::Node *node, std::map<TR::Node *, set<Entry>> &evaluatedNodeValues, int visitCount, int methodIndex)
{

   set<Entry> evaluatedValues;

   TR::Node *usefulNode = getUsefulNode(node);

   if (!usefulNode)
      return evaluatedValues;

   if (usefulNode->getVisitCount() >= visitCount)
   {
      // the node's been visited before - fetch its evaluated value
      evaluatedValues = evaluatedNodeValues[usefulNode];
      return evaluatedValues;
   }
   else
   {
      // node hasn't been visited, mark visited and evaluate
      usefulNode->setVisitCount(visitCount);

      TR::ILOpCodes opCode = usefulNode->getOpCodeValue();
      switch (opCode)
      {
      case TR::New:
      {
         // process new here
         // TODO: this needs to be a combination of the methodIndex-bci (use longint)
         Entry e = evaluateAllocate(usefulNode, methodIndex);
         evaluatedValues.insert(e);
         break;
      }

      case TR::astore:
      {
         // process store here
         // astore has a single child denoting an address
         TR::Node *storeChild = usefulNode->getFirstChild();
         evaluatedValues = evaluateNode(in, storeChild, evaluatedNodeValues, visitCount, methodIndex);
         // now we update the rho map for the symref
         // note that this is a strong update
         int storeSymRef = usefulNode->getSymbolReference()->getReferenceNumber();
         in->assign(storeSymRef, evaluatedValues);
         // TODO: do astore's need an evaluated value? can there be pointers to astore nodes?

         // if(_runtimeVerifierDiagnostics) in->print();
         break;
      }
      case TR::aload:
      {
         // process load here
         // an aload's evaluated value is simply the list of objects in the points-to set of its symref
         int loadSymRef = usefulNode->getSymbolReference()->getReferenceNumber();
         set<Entry> pointsToSet = in->getPointsToSet(loadSymRef);
         for (Entry entry : pointsToSet)
         {
            // TODO: use copy here
            evaluatedValues.insert(entry);
         }
         break;
      }

      case TR::aloadi:
      {
         TR::SymbolReference *symRef = usefulNode->getSymbolReference();
         bool isUnresolved = symRef->isUnresolved();
         IFDIAGPRINT << "isUnresolved = " << isUnresolved << endl;

         bool isShadow = symRef->getSymbol()->getKind() == TR::Symbol::IsShadow;
         IFDIAGPRINT << "isShadow = " << isShadow << endl;

         int cpIndex = symRef->getCPIndex();
         IFDIAGPRINT << "cp index = " << cpIndex << endl;

         if (/* !isUnresolved && */ isShadow && cpIndex > 0)
         {
            // this is most certainly a field access, until proven otherwise

            int32_t len;
            const char *fieldName = usefulNode->getSymbolReference()->getOwningMethod(_runtimeVerifierComp)->fieldNameChars(usefulNode->getSymbolReference()->getCPIndex(), len);

            IFDIAGPRINT << "field access for " << fieldName << endl;

            // receiver
            TR::Node *receiverNode = usefulNode->getFirstChild();
            set<Entry> receiverNodeVals = evaluateNode(in, receiverNode, evaluatedNodeValues, visitCount, methodIndex);

            for (Entry receiver : receiverNodeVals)
            {
               // we fetch the requested field for each of the receiver pointees, then union
               set<Entry> rhsPointees = in->getPointsToSet(receiver, fieldName);
               evaluatedValues.insert(rhsPointees.begin(), rhsPointees.end());
            }
         }

         break;
      }

      case TR::awrtbari:
      {
         // process field store
         // first we obtain the children of awrtbari
         // obviously we only process if the RHS of the field write is a ref type

         TR::Node *valueNode = usefulNode->getSecondChild();
         if (valueNode->getDataType() == TR::Address)
         {
            // receiver
            TR::Node *receiverNode = usefulNode->getFirstChild();
            set<Entry> receiverNodeVals = evaluateNode(in, receiverNode, evaluatedNodeValues, visitCount, methodIndex);

            // value
            set<Entry> valueNodeVals = evaluateNode(in, valueNode, evaluatedNodeValues, visitCount, methodIndex);

            TR::SymbolReference *symRef = usefulNode->getSymbolReference();
            bool isUnresolved = symRef->isUnresolved();
            IFDIAGPRINT << "isUnresolved = " << isUnresolved << endl;

            bool isShadow = symRef->getSymbol()->getKind() == TR::Symbol::IsShadow;
            IFDIAGPRINT << "isShadow = " << isShadow << endl;

            int cpIndex = symRef->getCPIndex();
            IFDIAGPRINT << "cp index = " << cpIndex << endl;

            if (/* !isUnresolved && */ isShadow && cpIndex > 0)
            {
               // this is most certainly a field access, until proven otherwise

               int32_t len;
               const char *fieldName = usefulNode->getSymbolReference()->getOwningMethod(_runtimeVerifierComp)->fieldNameChars(usefulNode->getSymbolReference()->getCPIndex(), len);

               IFDIAGPRINT << "field access for " << fieldName << endl;

               for (Entry receiver : receiverNodeVals)
               {
                  in->assign(receiver, fieldName, valueNodeVals);
               }
            }
            // fetch the field being written to
            //            // TODO - there is definitely a better way to do this! Look in Walker and TreeEvaluator
            //            // const char *fieldSig = usefulNode->getSymbolReference()->getName(_runtimeVerifierComp->getDebug());
            //            // char *field = strtok((char *)fieldSig, ". ");
            //            // field = strtok(NULL, ". ");
            //
            //            int32_t len;
            //            const char *fieldName = usefulNode->getSymbolReference()->getOwningMethod(_runtimeVerifierComp)->fieldNameChars(usefulNode->getSymbolReference()->getCPIndex(), len);
            //
            //            for (Entry receiverBCI : receiverNodeVals)
            //            {
            //               in->assign(receiverBCI, fieldName, valueNodeVals);
            //            }
         }
         else
         {
            // we do not care, this is not storing a ref type
         }

         break;
      }

      case TR::vcalli:
      case TR::icalli:
      case TR::lcalli:
      case TR::fcalli:
      case TR::dcalli:
      case TR::acalli:
      case TR::calli:
      case TR::icall:
      case TR::lcall:
      case TR::fcall:
      case TR::dcall:
      case TR::acall:
      case TR::call:
      case TR::vcall:
      {
         // usefulNode->getSymbol()->castToMethodSymbol()->isNonReturning();
         bool isHelperMethodCall = usefulNode->getSymbol()->castToMethodSymbol()->isHelper();
         // we do not want to process helper method calls (osr, for example)
         if (!isHelperMethodCall)
         {
            bool descendIntoMethod = true;
            const char *methodName = usefulNode->getSymbolReference()->getName(_runtimeVerifierComp->getDebug());
               //TODO : skip processing if called method is a library method
               string s = methodName;
               bool isLibraryMethod = s.rfind("java/", 0) == 0 || s.rfind("com/ibm/", 0) == 0 || s.rfind("sun/", 0) == 0 || s.rfind("openj9/", 0) == 0 || s.rfind("jdk/", 0) == 0;
               if(isLibraryMethod) {
                  cout << "bypassing " << s << " - a library method" << endl;
                  descendIntoMethod = false;
               }

            PointsToGraph *callSitePtg = new PointsToGraph(*in);
            // kill all the locals and return local (i.e. retain only the Heap)
            // TODO: confirm - does this simply mean set the Rho to empty map ?
            callSitePtg->killRho();
            callSitePtg->killArgs();
            callSitePtg->setBotReturn();

            if (descendIntoMethod && usefulNode->getSymbol()->isResolvedMethod())
            {
               TR::ResolvedMethodSymbol *methodSymbol = usefulNode->getSymbol()->castToResolvedMethodSymbol();
               string sig = methodSymbol->signature(_runtimeVerifierComp->trMemory());
               cout << sig << " is resolved" << endl;

               // TODO: called method is resolved. map the arguments and peek into it
               //               mapParameters(in, callSitePtg, usefulNode);

               // invokespecial (private instance calls, constructors, etc - labeled 'special' in the IL tree) and invokevirtual (public, package private, defaults,)
               //  pass the this-parm. However the trees are slightly different
               int argIndex = 0;
               int childIndex = 0;
               if (methodSymbol->isVirtual())
               {
                  // the first arg for a virtual call is the load of the VFT, we shall skip that
                  TR::Node *thisParmNode = usefulNode->getSecondChild();
                  set<Entry> thisParmValues = evaluateNode(in, thisParmNode, evaluatedNodeValues, visitCount, methodIndex);
                  callSitePtg->setArg(0, thisParmValues);

                  argIndex++;
                  childIndex += 2;
               }
               else if (methodSymbol->isSpecial())
               {
                  // no VFTs here, pick off the args directly
                  TR::Node *thisParmNode = usefulNode->getFirstChild();
                  set<Entry> thisParmValues = evaluateNode(in, thisParmNode, evaluatedNodeValues, visitCount, methodIndex);
                  callSitePtg->setArg(0, thisParmValues);

                  argIndex++;
                  childIndex++;
               }
               else if (methodSymbol->isStatic())
               {
                  // no VFT or this-param, pick off the args directly
                  argIndex++;
               }
               else if (methodSymbol->isInterface())
               {
                  // invokeinterface do not seem to get resolved - what do we do ??
                  // TODO: add an assert_fatal here
                  argIndex++;
               }

               // now we can pick off the rest of the arguments from the IL

               ListIterator<TR::ParameterSymbol> paramIterator(&(methodSymbol->getParameterList()));
               TR::SymbolReference *symRef;
               int paramSlotcount = methodSymbol->getNumParameterSlots();
               cout << sig << " method has " << paramSlotcount << " params " << endl;

               TR::ParameterSymbol *paramCursor = paramIterator.getFirst();
               if (methodSymbol->isVirtual() || methodSymbol->isSpecial())
               {
                  // the this-param is already mapped, so skip the first one
                  paramCursor = paramIterator.getNext();
               }

               for (; paramCursor != NULL; paramCursor = paramIterator.getNext())
               {
                  TR::SymbolReference *symRef;
                  // param at getSlot == 0 is the this-pointer
                  int paramSlot = paramCursor->getSlot();
                  symRef = methodSymbol->getParmSymRef(paramSlot);
                  cout << paramSlot << " ";
                  cout << (symRef->getSymbol()->getType().isAddress() ? "is address " : "is scalar ") << (symRef->isThisPointer() ? " is this pointer" : "") << endl;
                  if (symRef->getSymbol()->getType().isAddress())
                  {
                     cout << "attempting to map argIndex " << argIndex << endl;
                     TR::Node *argNode = usefulNode->getChild(childIndex);
                     set<Entry> argValues = evaluateNode(in, argNode, evaluatedNodeValues, visitCount, methodIndex);

                     callSitePtg->setArg(argIndex, argValues);
                  }
                  //               symRef->isThisPointer();
                  argIndex++;
                  childIndex++;
               }

               //   ListIterator<TR::ParameterSymbol> paramIterator(&(methodSymbol->getParameterList()));
               //   TR::SymbolReference *symRef;
               //   for (TR::ParameterSymbol *paramCursor = paramIterator.getFirst(); paramCursor != NULL; paramCursor = paramIterator.getNext())
               //   {
               //      // param at getSlot == 0 is the this-pointer
               //      int paramSlot = paramCursor->getSlot();
               //      symRef = methodSymbol->getParmSymRef(paramSlot);
               //      int32_t symRefNumber = symRef->getReferenceNumber();
               //
               //      if (_runtimeVerifierDiagnostics)
               //         cout << "the symref number corresponding to param " << paramCursor->getSlot() << " is " << symRefNumber << endl;
               //
               //      set <Entry> argsPointsTo = in->getArgPointsToSet(paramSlot);
               //      in->assign(symRefNumber, argsPointsTo);
               //   }

               if (_runtimeVerifierDiagnostics)
               {
                  cout << "callsite ptg mapped:" << endl;
                  callSitePtg->print();
               }

               cout << "peeking method " << sig << " isResolved = " << methodSymbol->isResolvedMethod() << endl;

               TR::ResolvedMethodSymbol *resolvedMethodSymbol = usefulNode->getSymbol()->getResolvedMethodSymbol();
               if (usefulNode->getSymbol()->isResolvedMethod())
               {

                  cout << forceCallsiteArgsForJITCInvocation.size() << endl;
                  forceCallsiteArgsForJITCInvocation.insert(pair<string, PointsToGraph *>(sig, callSitePtg));
                  cout << forceCallsiteArgsForJITCInvocation.size() << endl;
                  //                     TR_ASSERT_FATAL(forceCallsiteArgsForJITCInvocation.size() <= 1, "a maximum of 1 method can be forced");

                  bool ilGenFailed = NULL == resolvedMethodSymbol->getResolvedMethod()->genMethodILForPeekingEvenUnderMethodRedefinition(resolvedMethodSymbol, _runtimeVerifierComp, false);

                  // if(ilGenFailed) cout << "fatal IL gen failed!" << endl;
                  TR_ASSERT_FATAL(!ilGenFailed, "IL Gen failed, cannot peek into method");

                  _runtimeVerifierComp->dumpMethodTrees("Method tree about to peek", resolvedMethodSymbol);

                  _runtimeVerifierComp->dumpFlowGraph(resolvedMethodSymbol->getFlowGraph());

                  // given that the ILGen will run optimizations and force invocation of the algorithm by JIT compilation, do we even need this call?
                  //verifyStaticMethodInfo(visitCount, _runtimeVerifierComp, resolvedMethodSymbol, getFormattedCurrentClassName(resolvedMethodSymbol),
                                         //getFormattedCurrentMethodName(resolvedMethodSymbol), callSitePtg, false);
               }
               PointsToGraph *outPTG = verifiedMethodSummaries[sig];
               evaluatedValues = outPTG->getReturnPointsTo();
               if(_runtimeVerifierDiagnostics) {
                  cout << "callsite processing for " << sig << " completed, callsite PTG below" << endl;
                  outPTG->print();
               }
            }
            else
            {
               cout << "found an unresolved method " << methodName << endl;
               // TODO: method is not resolved, do
               // 1. set return to BOT
               // 2. summarize the reachable heap - this involves use of the escape map
               // bottomize all heap references reachable from the arguments
            }
         }
         break;
      }

         /*
               case TR::vcalli:
               case TR::icalli:
               case TR::lcalli:
               case TR::fcalli:
               case TR::dcalli:
               case TR::acalli:
               case TR::calli:
               {
         //usefulNode->getSymbol()->castToMethodSymbol()->isNonReturning();
                  bool isHelperMethodCall = usefulNode->getSymbol()->castToMethodSymbol()->isHelper();
                     if(!isHelperMethodCall) {
                     const char *methodName = usefulNode->getSymbolReference()->getName(_runtimeVerifierComp->getDebug());

                        PointsToGraph *callSitePtg = new PointsToGraph(*in);
                        //kill all the locals and return local (i.e. retain only the Heap)
                        //TODO: confirm - does this simply mean set the Rho to empty map ?
                        callSitePtg->killRho();
                        callSitePtg->setBotReturn();

                     if(usefulNode->getSymbol()->isResolvedMethod()) {
                        cout << "the method is resolved" << endl;
                        string sig = usefulNode->getSymbol()->castToResolvedMethodSymbol()->signature(_runtimeVerifierComp->trMemory());
                        cout << sig << " is resolved" << endl;

                        //TODO: called method is resolved. map the arguments and peek into it
                        if(_runtimeVerifierDiagnostics) {
                           cout << "return bottomized, parameters mapped, here is the in ptg" << endl;
                           callSitePtg->print();
                        }
                     } else {
                        cout << "found an unresolved method " << methodName << endl;
                        //TODO: method is not resolved, do
                        //1. set return to BOT
                        //2. set all fields of all arguments to BOT (this includes the receiver, if applicable) - this involves the escape map
                        //bottomize all heap references reachable from the arguments
                     }
                  }
                  break;

               }

               case TR::icall:
               case TR::lcall:
               case TR::fcall:
               case TR::dcall:
               case TR::acall:
               case TR::call:
               case TR::vcall:
               {
                  bool isHelperMethodCall = usefulNode->getSymbol()->castToMethodSymbol()->isHelper();
                     if(!isHelperMethodCall) {
                     const char *methodName = usefulNode->getSymbolReference()->getName(_runtimeVerifierComp->getDebug());

                     if(usefulNode->getSymbol()->isResolvedMethod()) {
                        cout << "the method is resolved" << endl;
                        string sig = usefulNode->getSymbol()->castToResolvedMethodSymbol()->signature(_runtimeVerifierComp->trMemory());
                        cout << sig << " is resolved" << endl;
                     } else {
                        cout << "found an unresolved method " << methodName << endl;
                     }
                  }
                  break;
               }
         */

      case TR::Return:
      case TR::lreturn:
      case TR::ireturn:
      case TR::dreturn:
      case TR::freturn:
      case TR::areturn:
      case TR::vreturn:
      {
         // handle the return value. We use a magic number (-99) to represent the pseudo-symref of the return var
         // also, we only need to worry about the areturn. So why do we have the others here? Maybe want to process some cleanup
         //   actions on encountering a return op

         if (opCode == TR::areturn)
         {
            set<Entry> returnPointees = evaluateNode(in, usefulNode->getFirstChild(), evaluatedNodeValues, visitCount, methodIndex);
            //TODO: this is wrong - take the meet with each return, below code is wrongly over-writing the return each time
            in->assignReturn(returnPointees);

            if (_runtimeVerifierDiagnostics)
            {
               cout << "processed areturn" << endl;
               in->print();
            }
         }
         break;
      }

      default:
      {
         //         TR_ASSERT_FATAL(true, "opcode %s not recognized", usefulNode->getOpCode().getName());
         break;
      }
      }
   }

   // TODO: update the evaluated values here, or in the caller? Lets do it here, for now
   evaluatedNodeValues[usefulNode] = evaluatedValues;
   return evaluatedValues;
}

void pseudoTopoSort(TR::Block *currentBlock, vector<TR::Block *> &gray, vector<TR::Block *> &black, stack<TR::Block *> &sorted)
{
   if (find(gray.begin(), gray.end(), currentBlock) != gray.end())
   {
      return;
   }
   else
   {
      gray.push_back(currentBlock);
   }

   TR::CFGEdgeList successors = currentBlock->getSuccessors();
   for (TR::CFGEdgeList::iterator successorIt = successors.begin(); successorIt != successors.end(); ++successorIt)
   {
      TR::Block *successorBlock = toBlock((*successorIt)->getTo());

      if (find(black.begin(), black.end(), successorBlock) != black.end())
      {
         continue;
      }
      else
      {
         pseudoTopoSort(successorBlock, gray, black, sorted);
      }
   }

   gray.erase(find(gray.begin(), gray.end(), currentBlock));
   black.push_back(currentBlock);
   sorted.push(currentBlock);
}

/*
 * Runs Points-To Analysis for the method represented by the supplied resolved method symbol.
 */
PointsToGraph *performRuntimePointsToAnalysis(PointsToGraph *inFlow, TR::ResolvedMethodSymbol *methodSymbol, int visitCount)
{
   if (_runtimeVerifierDiagnostics)
      inFlow->print();
   // instatiate the outFlow as a copy of the inFlow
   PointsToGraph *outFlow = new PointsToGraph(*inFlow);

   string methodSignature = methodSymbol->signature(_runtimeVerifierComp->trMemory());
   if (_runtimeVerifierDiagnostics)
   {
      cout << "performing runtime PTA for " << methodSignature << endl;
      cout << "in-PTG:" << endl;
      outFlow->print();
   }

   // TODO: 'kill' the locals, args and return of the caller.

   // load in the loop invariants for this method
   int methodIndex = getOrInsertMethodIndex(methodSignature);
   IFDIAGPRINT << "attempting to read loop invariant " << methodIndex << " " << methodSignature << endl;
   std::map<int, PointsToGraph> staticLoopInvariants = readLoopInvariant(methodIndex);

   //string callsiteInvariantFileName = "invariants/ci" + std::to_string(methodIndex) + ".txt";
   IFDIAGPRINT << "attempting to read callsite invariant " << methodIndex << " " << methodSignature << endl;
   PointsToGraph staticCallSiteInvariant = readCallsiteInvariant(methodIndex);

   // TODO: it'd be nice to encapsulate both of these into a context of sorts
   // a collection of all the in-PTGs, keyed by the bci of the instruction
   map<int, PointsToGraph *> ins;
   // a collection of all the out-PTGs, keyed by the bci of the instruction
   map<int, PointsToGraph *> outs;

   // a convenience collection of the out-PTGs of each basic block. Obviously this is the same as the out-PTG of the last viable statement in that block -
   // unfortunately there isn't a way to map them, out of the box
   map<TR::Block *, PointsToGraph *> basicBlockOuts;

   // we begin from the start node of the CFG
   // TODO: perform the topological sort of the CFG here, to identify the order in which the basic blocks are to be processed
   TR::CFG *cfg = methodSymbol->getFlowGraph();
   if (!cfg)
      cout << "cfg is null!" << endl;
   TR::Block *start = cfg->getStart()->asBlock();

   // perform a topological sort of the CFG to determine the order in which the basic blocks are to be processed
   vector<TR::Block *> gray;
   vector<TR::Block *> black;
   stack<TR::Block *> blockProcessingOrder;
   pseudoTopoSort(start, gray, black, blockProcessingOrder);

   //   queue<TR::Block *> workList;
   //   workList.push(start);

   // not technically needed. We can look to see if this block has an Out-PTG
   // set<int> visitedBlocks;
   std::map<TR::Node *, set<Entry>> evaluatedNodeValues;

   while (!blockProcessingOrder.empty())
   {
      TR::Block *currentBB = blockProcessingOrder.top();
      blockProcessingOrder.pop();

      int currentBBNumber = currentBB->getNumber();
      if (_runtimeVerifierDiagnostics)
         cout << "popped BB" << currentBBNumber << " from the worklist" << endl;

      // do we need to mark the block as visited?
      // if(visitedBlocks)

      PointsToGraph *inForBasicBlock;

      if (currentBB == start)
      {
         inForBasicBlock = inFlow;
      }
      else
      {
         inForBasicBlock = getPredecessorMeet(currentBB, basicBlockOuts);
      }

      PointsToGraph *localRunningPTG = inForBasicBlock;

      TR::TreeTop *tt = currentBB->getEntry();
      // its possible that there are no entry treetops for certain basic blocks
      // TODO: DOCUMENT THIS
      if (tt)
      {
         // now we iterate over the treetops in the basic block
         for (; tt; tt = tt->getNextRealTreeTop())
         {
            TR::Node *node = tt->getNode();
            IFDIAGPRINT << "*** now processing node n" << node->getGlobalIndex() << "n, with opcode " << node->getOpCode().getName() << endl;
            // unfortunately it appears that the Start and End nodes are also valid treetops.
            // TODO: is there a way around this check?
            if (node->getOpCodeValue() == TR::BBStart)
               continue;
            else if (node->getOpCodeValue() == TR::BBEnd)
               break;

            int nodeBCI = node->getByteCodeInfo().getByteCodeIndex();
            if (staticLoopInvariants.find(nodeBCI) != staticLoopInvariants.end())
            {
               IFDIAGPRINT << "found static loop invariant at bci " << nodeBCI << endl;
            }

            // if there is an invariant available for this bci, map it in
            // mapStaticInvariantToRunningPTG(invariantPTG, localRunningPTG);

            // update the node's visit count, if it is less than the current visit count;
            //  if(node->getVisitCount() < visitCount) {
            //     node->setVisitCount(visitCount);
            //     cout << node->getGlobalIndex() << " visited first time" << endl;
            //  } else {
            //     //this node has already been visited before!
            //     cout << node->getGlobalIndex() << " visited already!" << endl;
            //     continue;
            //  }

            // if there is an interesting node, we evaluate it. This will also update the rho/sigma maps where applicable
            set<Entry> evaluatedValuesForNode = evaluateNode(localRunningPTG, node, evaluatedNodeValues, visitCount, methodIndex);
            // quick test to make sure values are persisting b/w calls
            // UPDATE: they are!
            //            if(_runtimeVerifierDiagnostics) {
            //	           for(Entry e : evaluatedValuesForNode) {
            //	              cout << e.getString() << endl;
            //	           }
            //            }

            if (_runtimeVerifierDiagnostics)
            {
               cout << "processed evaluate node" << endl;
               localRunningPTG->print();
            }

            // lets store away the running ptg as the out of the current bci
            // by design of the algorithm, the outs of any BB will/should never change
            outs[nodeBCI] = new PointsToGraph(*localRunningPTG);
         }
      }

      /* we now look at the successors of this block, if any successor has already been
       * analyzed, we check for invariance
       *
       * the subsumes relation is
       * if static invariant is present:
       *    ptg_invariant <subsumes> ( ptg_in_of_successor <meet> ptg_out_of_current )
       * else
       *    ptg_in_of_successor <subsumes> ptg_out_of_current
       */
      for (TR::CFGEdgeList::iterator successorIt = currentBB->getSuccessors().begin(); successorIt != currentBB->getSuccessors().end(); ++successorIt)
      {
         TR::Block *successorBlock = toBlock((*successorIt)->getTo());
         if (basicBlockOuts.find(successorBlock) != basicBlockOuts.end())
         {
            cout << "BB " << successorBlock->getNumber() << "already analyzed, invariance check, current BB is " << currentBBNumber << endl;

            bool staticInvariantExists = false;
            if (staticInvariantExists)
            {
            }
            else
            {
               PointsToGraph *ptgRunning = getPredecessorMeet(successorBlock, basicBlockOuts);
               bool subsumes = ptgRunning->subsumes(ptgRunning);
               cout << "subsumes check returned " << subsumes << endl;
            }
         }
      }

      // if we have reached here, the localRunningPTG now represents the out-flow of the current basic block. Store it away for later use
      basicBlockOuts[currentBB] = localRunningPTG;

      // re-enable this for verification testing
      // if(out != prevOut) {

      // the below code for adding successors is no longer needed, since the order of processing is determined before hand by the topological sort
      //      TR::CFGEdgeList successors = currentBB->getSuccessors();
      //      for (TR::CFGEdgeList::iterator successorIt = successors.begin(); successorIt != successors.end(); ++successorIt)
      //      {
      //         TR::Block *successorBlock = toBlock((*successorIt)->getTo());
      //
      //         workList.push(successorBlock);
      //      }
      // }
   }

   TR::Block *end = cfg->getEnd()->asBlock();

   PointsToGraph *outForMethod = basicBlockOuts[end];

   //save this away as the summary for this method!
   verifiedMethodSummaries[methodSignature] = outForMethod;
   if (_runtimeVerifierDiagnostics)
   {
      cout << "completed runtime PTA for " << methodSignature << endl;
      cout << "out-PTG:" << endl;
      outForMethod->print();
   }
   return outForMethod;
}

// the default values here imply that verify() has been invoked by the JIT-C and not the runtime verification algorithm
PointsToGraph *verifyStaticMethodInfo(int visitCount, TR::Compilation *comp = NULL, TR::ResolvedMethodSymbol *methodSymbol = NULL,
                                      std::string className = "", std::string methodName = "", PointsToGraph *inFlow = NULL, bool isInvokedByJITC = true)
{

   string methodSignature = methodSymbol->signature(_runtimeVerifierComp->trMemory());

   if (isInvokedByJITC && forceCallsiteArgsForJITCInvocation.find(methodSignature) != forceCallsiteArgsForJITCInvocation.end())
   {
      // this method's verification was technically invoked via callsite descent, but due to
      //  the JITC's plumbing, we are in an InvokedbyJITC mode

      isInvokedByJITC = false;
      inFlow = forceCallsiteArgsForJITCInvocation[methodSignature];
      forceCallsiteArgsForJITCInvocation.clear();
   }

   cout << "analyzing method " << methodSignature << endl;
   if (!_runtimeVerifierDiagnostics)
      _runtimeVerifierDiagnostics = feGetEnv("TR_runtimeVerifyDiag") != NULL;

   PointsToGraph *outFlow = new PointsToGraph();

   // if we are invoked by the algorithm itself (by descending into call site, then the class/methodname will be populated)
   //if (isInvokedByJITC)
   //{
      //className = getFormattedCurrentClassName(comp->getMethodSymbol());
      //methodName = getFormattedCurrentMethodName(comp->getMethodSymbol());
   //}

   // TODO: use the standardized method signature here, no need for another specially formatted string
   //std::string sig = className + "." + methodName;
   bool analyzed = false;
   if (_runtimeVerifiedMethods.find(methodSignature) != _runtimeVerifiedMethods.end())
   {
      // this method has already been analyzed
      cout << "\talready analyzed" << endl;
      analyzed = true;
   }
   else
      _runtimeVerifiedMethods.insert(methodSignature);

   // a guarantee that each method is processed at most once
   if (!analyzed)
   {

      // TODO: remember that checking for emptiness isn't the way to lazy-load a map
      // TODO: shouldn't this be done at a level one level higher?
      if (_methodIndices.empty())
         _methodIndices = readMethodIndices();

      // cout << methodSymbol->getName();
      // cout << methodSymbol->getMethod()->nameChars() << endl;
      string methodSignature = methodSymbol->signature(_runtimeVerifierComp->trMemory());

      // maintain the PTG in a before-after format. The flow function will transform the "before" to the "after"
      // std::map<int, PointsToGraph> ptgsBefore;
      // std::map<int, PointsToGraph> ptgsAfter;
      // a convenience map to track the out-ptgs of each basic block. Makes propagation easier. The key is the basic block number
      // std::map<int, PointsToGraph> outPTGs;
      // std::map<int, PointsToGraph> stackSlotMappedInvariantPTGs;
      // if the analysis is invoked by the JIT-C, all the relevant information will be available on the compilation object

      if (isInvokedByJITC)
      {

         // inFlow is guaranteed to be null if invoked by the JITC, so we initialize it
         inFlow = new PointsToGraph();

         if (_runtimeVerifierDiagnostics)
            cout << "runtime verification of method " << methodSignature << ", index " << getOrInsertMethodIndex(methodSignature) << " invoked by JIT-C" << endl;

         // verify has been invoked by the JIT-C - so we need to bottomize the incoming arguments
         bottomizeParameters(methodSymbol, inFlow);
         if (_runtimeVerifierDiagnostics)
            inFlow->print();
      }
      else
      {
         if (_runtimeVerifierDiagnostics)
            cout << "runtime verification of method " << methodSignature << ", index " << getOrInsertMethodIndex(methodSignature) << " invoked by callsite descent" << endl;
         // verify() was invoked by the verification algorithm, so all the required data points should be available.
         // TODO: is there any housekeeping unique to this scenario?
         mapParametersIn(methodSymbol, inFlow);
         if (_runtimeVerifierDiagnostics)
         {
            cout << "parameters mapped for method " << methodSignature << endl;
            inFlow->print();
         }
      }

      // now that we have the inflow adjusted, proceed to perform the runtime points to analysis for this method
      outFlow = performRuntimePointsToAnalysis(inFlow, methodSymbol, visitCount);
   }

   return outFlow;
}

// StaticPtg top() {
//    StaticPtg ptg;

//    return ptg;
// }

// void performRuntimeVerification(TR::Compilation *comp)
//{
//
//    std::string currentClassName = getFormattedCurrentClassName(comp->getMethodSymbol());
//    std::string currentMethodName = getFormattedCurrentMethodName(comp->getMethodSymbol());
//    std::string sig = currentClassName + "." + currentMethodName;
//    if (_runtimeVerifiedMethods.find(sig) != _runtimeVerifiedMethods.end())
//       // this method has already been analyzed
//       return;
//    else
//       _runtimeVerifiedMethods.insert(sig);
//
//    // std::cout << "running performRuntimeVerification for " << currentMethodName << endl;
//
//    TR::CFG *cfg = comp->getFlowGraph();
//
//    // traceMsg(comp, "CFG for callee method %s\n", currentMethodName);
//    comp->dumpFlowGraph(cfg);
//    comp->dumpMethodTrees("from OMR::Optimizer::performRuntimeVerification");
//
//    // if (trace())
//    {
//       traceMsg(comp, "*****************************************************\n");
//       traceMsg(comp, "Beginning Runtime Verification for %s.%s\n", currentClassName.c_str(), currentMethodName.c_str());
//
//       // dump the current symbol table, to establish a baseline
//
//       TR::SymbolReferenceTable *tab = comp->getSymRefTab();
//       for (int i = tab->getIndexOfFirstSymRef(); i < tab->getNumSymRefs(); i++)
//       {
//
//          TR::SymbolReference *sym = tab->getSymRef(i);
//
//          if (sym != NULL && sym->getSymbol()->getType().isAddress())
//             traceMsg(comp, "Symref #%d - CPIndex %d, isTemporary: %d, isAuto: %d\n", sym->getReferenceNumber(), sym->getCPIndex(), sym->isTemporary(comp), sym->getSymbol()->isAuto());
//       }
//    }
//
//    // recursively invoke the verification algorithm (calls itself at each callsite to analyze the called method)
//    // verifyStaticMethodInfo(currentClassName, currentMethodName, cfg, comp);
//
//    // if (trace())
//    {
//       traceMsg(comp, "*****************************************************");
//    }
// }

int32_t OMR::Optimizer::performOptimization(const OptimizationStrategy *optimization, int32_t firstOptIndex, int32_t lastOptIndex, int32_t doTiming)
{
   bool performRuntimeVerify = feGetEnv("TR_PerformRuntimeVerify") != NULL;
   if (performRuntimeVerify)
   {
      if (!_runtimeVerifierComp)
         _runtimeVerifierComp = comp();

      comp()->dumpMethodTrees("Trees before performRuntimeVerification");
      // cout << "visit count before inc " << comp()->getVisitCount();
      comp()->incVisitCount();
      // cout << "visit count after inc " << comp()->getVisitCount();
      //  when invoked from the JIT, best we can do is supply the compilation object
      verifyStaticMethodInfo(comp()->getVisitCount(), comp(), comp()->getMethodSymbol());
   }

   // comp()->getVisitCount(); //returns the latest visit count
   // comp()->incVisitCount(); //increments the visit count

   OMR::Optimizations optNum = optimization->_num;
   TR::OptimizationManager *manager = getOptimization(optNum);
   TR_ASSERT(manager != NULL, "Optimization manager should have been initialized for %s.",
             getOptimizationName(optNum));

   comp()->reportAnalysisPhase(BEFORE_OPTIMIZATION);
   breakForTesting(1010);

   int32_t optIndex = comp()->getOptIndex() + 1; // +1 because we haven't incremented yet at this point, becuase we're not sure we should
   // Determine whether or not to do this optimization
   //
   bool doThisOptimization = false;
   bool doThisOptimizationIfEnabled = false;
   bool mustBeDone = false;
   bool justSetLastRun = false;
   switch (optimization->_options)
   {
   case Always:
      doThisOptimization = true;
      break;

   case IfLoops:
      if (comp()->mayHaveLoops())
         doThisOptimization = true;
      break;

   case IfMoreThanOneBlock:
      if (hasMoreThanOneBlock(comp()))
         doThisOptimization = true;
      break;

   case IfOneBlock:
      if (!hasMoreThanOneBlock(comp()))
         doThisOptimization = true;
      break;

   case IfLoopsMarkLastRun:
      if (comp()->mayHaveLoops())
         doThisOptimization = true;
      TR_ASSERT(optNum < OMR::numOpts, "No current support for marking groups as last (optNum=%d,numOpt=%d\n", optNum, OMR::numOpts); // make sure we didn't mark groups
      manager->setLastRun(true);
      justSetLastRun = true;
      break;

   case IfNoLoops:
      if (!comp()->mayHaveLoops())
         doThisOptimization = true;
      break;

   case IfProfiling:
      if (comp()->isProfilingCompilation())
         doThisOptimization = true;
      break;

   case IfNotProfiling:
      if (!comp()->isProfilingCompilation() || debug("ignoreIfNotProfiling"))
         doThisOptimization = true;
      break;

   case IfNotJitProfiling:
      if (comp()->getProfilingMode() != JitProfiling)
         doThisOptimization = true;
      break;

   case IfNews:
      if (comp()->hasNews())
         doThisOptimization = true;
      break;

   case IfOptServer:
      if (comp()->isOptServer())
         doThisOptimization = true;
      break;

   case IfMonitors:
      if (comp()->getMethodSymbol()->mayContainMonitors())
         doThisOptimization = true;
      break;

   case IfEnabledAndMonitors:
      if (manager->requested() && comp()->getMethodSymbol()->mayContainMonitors())
         doThisOptimization = true;
      break;

   case IfEnabledAndOptServer:
      if (manager->requested() &&
          comp()->isOptServer())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

#ifdef J9_PROJECT_SPECIFIC
   case IfNotClassLoadPhase:
      if (!comp()->getPersistentInfo()->isClassLoadingPhase() ||
          comp()->getOption(TR_DontDowngradeToCold))
         doThisOptimization = true;
      break;

   case IfNotClassLoadPhaseAndNotProfiling:
      if ((!comp()->getPersistentInfo()->isClassLoadingPhase() ||
           comp()->getOption(TR_DontDowngradeToCold)) &&
          (!comp()->isProfilingCompilation() || debug("ignoreIfNotProfiling")))
         doThisOptimization = true;
      break;
#endif

   case IfEnabledAndLoops:
      if (comp()->mayHaveLoops() && manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

   case IfEnabledAndMoreThanOneBlock:
      if (hasMoreThanOneBlock(comp()) && manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

   case IfEnabledAndMoreThanOneBlockMarkLastRun:
      if (hasMoreThanOneBlock(comp()) && manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      TR_ASSERT(optNum < OMR::numOpts, "No current support for marking groups as last (optNum=%d,numOpt=%d\n", optNum, OMR::numOpts); // make sure we didn't mark groups
      manager->setLastRun(true);
      justSetLastRun = true;
      break;

   case IfEnabledAndNoLoops:
      if (!comp()->mayHaveLoops() && manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

   case IfNoLoopsOREnabledAndLoops:
      if (!comp()->mayHaveLoops() || manager->requested())
      {
         if (comp()->mayHaveLoops())
            doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

   case IfEnabledAndProfiling:
      if (comp()->isProfilingCompilation() && manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

   case IfEnabledAndNotProfiling:
      if (!comp()->isProfilingCompilation() && manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

   case IfEnabledAndNotJitProfiling:
      if (comp()->getProfilingMode() != JitProfiling && manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

   case IfLoopsAndNotProfiling:
      if (comp()->mayHaveLoops() && !comp()->isProfilingCompilation())
         doThisOptimization = true;
      break;

   case MustBeDone:
      mustBeDone = doThisOptimization = true;
      break;

   case IfFullInliningUnderOSRDebug:
      if (comp()->getOption(TR_FullSpeedDebug) && comp()->getOption(TR_EnableOSR) && comp()->getOption(TR_FullInlineUnderOSRDebug))
         doThisOptimization = true;
      break;

   case IfNotFullInliningUnderOSRDebug:
      if (comp()->getOption(TR_FullSpeedDebug) && (!comp()->getOption(TR_EnableOSR) || !comp()->getOption(TR_FullInlineUnderOSRDebug)))
         doThisOptimization = true;
      break;

   case IfOSR:
      if (comp()->getOption(TR_EnableOSR))
         doThisOptimization = true;
      break;

   case IfVoluntaryOSR:
      if (comp()->getOption(TR_EnableOSR) && comp()->getOSRMode() == TR::voluntaryOSR)
         doThisOptimization = true;
      break;

   case IfInvoluntaryOSR:
      if (comp()->getOption(TR_EnableOSR) && comp()->getOSRMode() == TR::involuntaryOSR)
         doThisOptimization = true;
      break;

   case IfEnabled:
      if (manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      break;

   case IfEnabledMarkLastRun:
      if (manager->requested())
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
      TR_ASSERT(optNum < OMR::numOpts, "No current support for marking groups as last (optNum=%d,numOpt=%d\n", optNum, OMR::numOpts); // make sure we didn't mark groups
      manager->setLastRun(true);
      justSetLastRun = true;
      break;

   case IfAOTAndEnabled:
   {
      bool enableColdCheapTacticalGRA = comp()->getOption(TR_EnableColdCheapTacticalGRA);
      bool disableAOTColdCheapTacticalGRA = comp()->getOption(TR_DisableAOTColdCheapTacticalGRA);

      if ((comp()->compileRelocatableCode() || enableColdCheapTacticalGRA) && manager->requested() && !disableAOTColdCheapTacticalGRA)
      {
         doThisOptimizationIfEnabled = true;
         doThisOptimization = true;
      }
   }
   break;

   case IfMethodHandleInvokes:
   {
      if (comp()->getMethodSymbol()->hasMethodHandleInvokes() && !comp()->getOption(TR_DisableMethodHandleInvokeOpts))
         doThisOptimization = true;
   }
   break;

   case IfNotQuickStart:
   {
      if (!comp()->getOptions()->isQuickstartDetected())
      {
         doThisOptimization = true;
      }
      break;
   }

   case IfEAOpportunitiesMarkLastRun:
      getOptimization(optNum)->setLastRun(true);
      justSetLastRun = true;
      // fall through
   case IfEAOpportunities:
   case IfEAOpportunitiesAndNotOptServer:
   {
      if (comp()->getMethodSymbol()->hasEscapeAnalysisOpportunities())
      {
         if ((optimization->_options == IfEAOpportunitiesAndNotOptServer) && comp()->isOptServer())
         {
            // don't enable
         }
         else
         {
            doThisOptimization = true;
         }
      }
      break;
   }
   case IfAggressiveLiveness:
   {
      if (comp()->getOption(TR_EnableAggressiveLiveness))
      {
         doThisOptimization = true;
      }
      break;
   }
   case IfVectorAPI:
   {
      if (comp()->getMethodSymbol()->hasVectorAPI() &&
          !comp()->getOption(TR_DisableVectorAPIExpansion))
         doThisOptimization = true;
   }
   break;
   case MarkLastRun:
      doThisOptimization = true;
      TR_ASSERT(optNum < OMR::numOpts, "No current support for marking groups as last (optNum=%d,numOpt=%d\n", optNum, OMR::numOpts); // make sure we didn't mark groups
      manager->setLastRun(true);
      justSetLastRun = true;
      break;
   default:
      TR_ASSERT(0, "unexpection optimization flags");
   }

   if (doThisOptimizationIfEnabled && manager->getRequestedBlocks()->isEmpty())
      doThisOptimization = false;

   int32_t actualCost = 0;
   static int32_t optDepth = 1;

   TR_FrontEnd *fe = comp()->fe();

   // If this is the start of an optimization subGroup, perform the
   // optimizations in the subgroup.
   //
   if (optNum > OMR::numOpts && doThisOptimization)
   {
      if (comp()->getOption(TR_TraceOptDetails) || comp()->getOption(TR_TraceOpts))
      {
         if (comp()->isOutermostMethod())
            traceMsg(comp(), "%*s<optgroup name=%s>\n", optDepth * 3, " ", manager->name());
      }

      optDepth++;

      // Find the subgroup. It is either referenced directly from this
      // optimization or picked up from the table of groups using the
      // optimization number.
      //
      manager->setRequested(false);

      if (optNum == loopVersionerGroup && getOptimization(lastLoopVersionerGroup) != NULL)
         getOptimization(lastLoopVersionerGroup)->setRequested(false);

      const OptimizationStrategy *subGroup = ((TR::OptimizationManager *)manager)->groupOfOpts();
      const OptimizationStrategy *origSubGroup = subGroup;
      int32_t numIters = 0;

      while (1)
      {
         // Perform the optimizations in the subgroup
         //
         while (subGroup->_num != endGroup && subGroup->_num != endOpts)
         {
            actualCost += performOptimization(subGroup, firstOptIndex, lastOptIndex, doTiming);
            subGroup++;
         }

         numIters++;

         if (optNum == eachLocalAnalysisPassGroup)
         {
            const OptimizationStrategy *currSubGroup = subGroup;
            subGroup = origSubGroup;
            bool blocksArePending = false;
            while (subGroup->_num != endGroup && subGroup->_num != endOpts)
            {
               OMR::Optimizations optNum = subGroup->_num;
               if (!manager->getRequestedBlocks()->isEmpty())
               {
                  blocksArePending = true;
                  break;
               }
               subGroup++;
            }

            subGroup = currSubGroup;
            if (!blocksArePending ||
                (numIters >= MAX_LOCAL_OPTS_ITERS))
            {
               break;
            }
            else
               subGroup = origSubGroup;
         }
         else
            break;
      }

      optDepth--;

      if (comp()->getOption(TR_TraceOptDetails) || comp()->getOption(TR_TraceOpts))
      {
         if (comp()->isOutermostMethod())
            traceMsg(comp(), "%*s</optgroup>\n", optDepth * 3, " ");
      }

      return actualCost;
   }

   //
   // This is a real optimization.
   //
   TR::RegionProfiler rp(comp()->trMemory()->heapMemoryRegion(), *comp(), "opt/%s/%s", comp()->getHotnessName(comp()->getMethodHotness()),
                         getOptimizationName(optNum));

   if (comp()->isOutermostMethod())
      comp()->incOptIndex(); // Note that we count the opt even if we're not doing it, to keep the opt indexes more stable

   if (!doThisOptimization)
   {
      if (!manager->requested() &&
          !manager->getRequestedBlocks()->isEmpty())
      {
         TR_ASSERT(0, "Opt is disabled but blocks are still present\n");
      }
      return 0;
   }

   if (mustBeDone ||
       (optIndex >= firstOptIndex && optIndex <= lastOptIndex))
   {
      bool needTreeDump = false;
      bool needStructureDump = false;

      if (!isEnabled(optNum))
         return 0;

      TR::SimpleRegex *regex = comp()->getOptions()->getDisabledOpts();
      if (regex && TR::SimpleRegex::match(regex, optIndex))
         return 0;

      if (regex && TR::SimpleRegex::match(regex, manager->name()))
         return 0;

      // actually doing optimization
      regex = comp()->getOptions()->getBreakOnOpts();
      if (regex && TR::SimpleRegex::match(regex, optIndex))
         TR::Compiler->debug.breakPoint();

      TR::Optimization *opt = manager->factory()(manager);

      // Do any opt specific checks before analysis/opt is run
      if (!opt->shouldPerform())
      {
         delete opt;
         return 0;
      }

      if (comp()->getOption(TR_TraceOptDetails))
      {
         if (comp()->isOutermostMethod())
            getDebug()->printOptimizationHeader(comp()->signature(), manager->name(), optIndex, optimization->_options == MustBeDone);
      }

      if (comp()->getOption(TR_TraceOpts))
      {
         if (comp()->isOutermostMethod())
            traceMsg(comp(), "%*s%s\n", optDepth * 3, " ", manager->name());
      }

      if (!_aliasSetsAreValid && !manager->getDoesNotRequireAliasSets())
      {
         TR::Compilation::CompilationPhaseScope buildingAliases(comp());
         comp()->reportAnalysisPhase(BUILDING_ALIASES);
         breakForTesting(1020);
         dumpOptDetails(comp(), "   (Building alias info)\n");
         comp()->getSymRefTab()->aliasBuilder.createAliasInfo();
         _aliasSetsAreValid = true;
         ++actualCost;
      }
      breakForTesting(1021);

      if (manager->getRequiresUseDefInfo() || manager->getRequiresValueNumbering())
         manager->setRequiresStructure(true);

      if (manager->getRequiresStructure() && !comp()->getFlowGraph()->getStructure())
      {
         TR::Compilation::CompilationPhaseScope buildingStructure(comp());
         comp()->reportAnalysisPhase(BUILDING_STRUCTURE);
         breakForTesting(1030);
         dumpOptDetails(comp(), "   (Doing structural analysis)\n");

#ifdef OPT_TIMING
         TR_SingleTimer myTimer;
         if (doTiming)
         {
            myTimer.initialize("structural analysis", trMemory());
            myTimer.startTiming(comp());
         }
#endif

         actualCost += doStructuralAnalysis();

         if (_firstTimeStructureIsBuilt && comp()->getFlowGraph()->getStructure())
         {
            _firstTimeStructureIsBuilt = false;
            _numLoopsInMethod = 0;
            countNumberOfLoops(comp()->getFlowGraph()->getStructure());
            // dumpOptDetails(comp(), "Number of loops in the cfg = %d\n", _numLoopsInMethod);

            if (!comp()->getOption(TR_ProcessHugeMethods) && (_numLoopsInMethod >= (HIGH_LOOP_COUNT - 25)))
               _disableLoopOptsThatCanCreateLoops = true;
            _numLoopsInMethod = 0;
         }

         needStructureDump = true;

#ifdef OPT_TIMING
         if (doTiming)
         {
            myTimer.stopTiming(comp());
            statStructuralAnalysisTiming.update((double)myTimer.timeTaken() * 1000.0 / TR::Compiler->vm.getHighResClockResolution());
         }
#endif
      }
      breakForTesting(1031);

      if (manager->getStronglyPrefersGlobalsValueNumbering() &&
          getUseDefInfo() && !getUseDefInfo()->hasGlobalsUseDefs() &&
          !cantBuildGlobalsUseDefInfo())
      {
         // We would strongly prefer global usedef info, but we only have
         // local usedef info. We can build global usedef info so force a
         // rebuild.
         //
         setUseDefInfo(NULL);
      }

      if (manager->getDoesNotRequireLoadsAsDefsInUseDefs() &&
          getUseDefInfo() && getUseDefInfo()->hasLoadsAsDefs())
      {
         setUseDefInfo(NULL);
      }

      if (!manager->getDoesNotRequireLoadsAsDefsInUseDefs() &&
          getUseDefInfo() && !getUseDefInfo()->hasLoadsAsDefs())
      {
         setUseDefInfo(NULL);
      }

      TR_UseDefInfo *useDefInfo;
      if (manager->getRequiresGlobalsUseDefInfo() || manager->getRequiresGlobalsValueNumbering())
      {
         // We need global usedef info. If it doesn't exist but can be built,
         // build it.
         //
         if (!cantBuildGlobalsUseDefInfo() &&
             (!getUseDefInfo() || !getUseDefInfo()->hasGlobalsUseDefs()))
         {
            TR::Compilation::CompilationPhaseScope buildingUseDefs(comp());
            comp()->reportAnalysisPhase(BUILDING_USE_DEFS);
            breakForTesting(1040);
#ifdef OPT_TIMING
            TR_SingleTimer myTimer;
            if (doTiming)
            {
               myTimer.initialize("use defs (for globals definitely)", trMemory());
               myTimer.startTiming(comp());
            }
#endif

            LexicalTimer t("use defs (for globals definitely)", comp()->phaseTimer());
            TR::LexicalMemProfiler mp("use defs (for globals definitely)", comp()->phaseMemProfiler());
            useDefInfo = createUseDefInfo(comp(),
                                          true,  // requiresGlobals
                                          false, // prefersGlobals
                                          !manager->getDoesNotRequireLoadsAsDefsInUseDefs(),
                                          manager->getCannotOmitTrivialDefs(),
                                          false, // conversionRegsOnly
                                          true); // doCompletion

#ifdef OPT_TIMING
            if (doTiming)
            {
               myTimer.stopTiming(comp());
               statUseDefsTiming.update((double)myTimer.timeTaken() * 1000.0 / TR::Compiler->vm.getHighResClockResolution());
            }
#endif

            if (useDefInfo->infoIsValid())
            {
               setUseDefInfo(useDefInfo);
            }
            else
               // release storage for failed _useDefInfo
               delete useDefInfo;

            actualCost += 10;
            needTreeDump = true;
         }
      }

      else if (manager->getRequiresUseDefInfo() || manager->getRequiresValueNumbering())
      {
         if (!cantBuildLocalsUseDefInfo() && !getUseDefInfo())
         {
            TR::Compilation::CompilationPhaseScope buildingUseDefs(comp());
            comp()->reportAnalysisPhase(BUILDING_USE_DEFS);
            breakForTesting(1050);
#ifdef OPT_TIMING
            TR_SingleTimer myTimer;
            if (doTiming)
            {
               myTimer.initialize("use defs (for globals possibly)", trMemory());
               myTimer.startTiming(comp());
            }
#endif
            LexicalTimer t("use defs (for globals possibly)", comp()->phaseTimer());
            TR::LexicalMemProfiler mp("use defs (for globals possibly)", comp()->phaseMemProfiler());
            useDefInfo = createUseDefInfo(comp(),
                                          false, // requiresGlobals
                                          manager->getPrefersGlobalsUseDefInfo() || manager->getPrefersGlobalsValueNumbering(),
                                          !manager->getDoesNotRequireLoadsAsDefsInUseDefs(),
                                          manager->getCannotOmitTrivialDefs(),
                                          false, // conversionRegsOnly
                                          true); // doCompletion

#ifdef OPT_TIMING
            if (doTiming)
            {
               myTimer.stopTiming(comp());
               statUseDefsTiming.update((double)myTimer.timeTaken() * 1000.0 / TR::Compiler->vm.getHighResClockResolution());
            }
#endif

            if (useDefInfo->infoIsValid())
            {
               setUseDefInfo(useDefInfo);
            }
            else
               // release storage for failed _useDefInfo
               delete useDefInfo;

            actualCost += 10;
            needTreeDump = true;
         }
      }

      TR_ValueNumberInfo *valueNumberInfo;
      if (manager->getRequiresGlobalsValueNumbering())
      {
         // We need global value number info.
         // If it doesn't exist but can be built, build it.
         //
         if (!cantBuildGlobalsValueNumberInfo() &&
             (!getValueNumberInfo() || !getValueNumberInfo()->hasGlobalsValueNumbers()))
         {
            TR::Compilation::CompilationPhaseScope buildingValueNumbers(comp());
            comp()->reportAnalysisPhase(BUILDING_VALUE_NUMBERS);
            breakForTesting(1060);
#ifdef OPT_TIMING
            TR_SingleTimer myTimer;
            if (doTiming)
            {
               myTimer.initialize("global value numbering (for globals definitely)", trMemory());
               myTimer.startTiming(comp());
            }
#endif

            valueNumberInfo = createValueNumberInfo(true, false);

#ifdef OPT_TIMING
            if (doTiming)
            {
               myTimer.stopTiming(comp());
               statGlobalValNumTiming.update((double)myTimer.timeTaken() * 1000.0 / TR::Compiler->vm.getHighResClockResolution());
            }
#endif

            if (valueNumberInfo->infoIsValid())
               setValueNumberInfo(valueNumberInfo);
            actualCost += 10;
            needTreeDump = true;
         }
      }

      else if (manager->getRequiresValueNumbering())
      {
         if (!cantBuildLocalsValueNumberInfo() && !getValueNumberInfo())
         {
            TR::Compilation::CompilationPhaseScope buildingValueNumbers(comp());
            comp()->reportAnalysisPhase(BUILDING_VALUE_NUMBERS);
            breakForTesting(1070);
#ifdef OPT_TIMING
            TR_SingleTimer myTimer;
            if (doTiming)
            {
               myTimer.initialize("global value numbering (for globals possibly)", trMemory());
               myTimer.startTiming(comp());
            }
#endif

            valueNumberInfo = createValueNumberInfo(false, manager->getPrefersGlobalsValueNumbering());

#ifdef OPT_TIMING
            if (doTiming)
            {
               myTimer.stopTiming(comp());
               statGlobalValNumTiming.update((double)myTimer.timeTaken() * 1000.0 / TR::Compiler->vm.getHighResClockResolution());
            }
#endif
            if (valueNumberInfo->infoIsValid())
               setValueNumberInfo(valueNumberInfo);
            actualCost += 10;
            needTreeDump = true;
         }
      }

      if (manager->getRequiresAccurateNodeCount())
      {
         TR::Compilation::CompilationPhaseScope buildingAccurateNodeCount(comp());
         comp()->reportAnalysisPhase(BUILDING_ACCURATE_NODE_COUNT);
         breakForTesting(1080);
         comp()->generateAccurateNodeCount();
      }

      // dumpOptDetails(comp(), "\n");

#ifdef OPT_TIMING
      if (*(statOptTiming[optNum].getName()) == 0) // has no name yet
         statOptTiming[optNum].setName(manager->name());
#endif

#ifdef OPT_TIMING
      TR_SingleTimer myTimer;
      if (doTiming)
      {
         myTimer.initialize(manager->name(), trMemory());
         myTimer.startTiming(comp());
      }
#endif
      LexicalTimer t(manager->name(), comp()->phaseTimer());
      TR::LexicalMemProfiler mp(manager->name(), comp()->phaseMemProfiler());

      int32_t origSymRefCount = comp()->getSymRefCount();
      int32_t origNodeCount = comp()->getNodeCount();
      int32_t origCfgNodeCount = comp()->getFlowGraph()->getNextNodeNumber();
      int32_t origOptMsgIndex = self()->getOptMessageIndex();

      if (comp()->isOutermostMethod() && (comp()->getFlowGraph()->getMaxFrequency() < 0) && !manager->getDoNotSetFrequencies())
      {
         TR::Compilation::CompilationPhaseScope buildingFrequencies(comp());
         comp()->reportAnalysisPhase(BUILDING_FREQUENCIES);
         breakForTesting(1100);
         comp()->getFlowGraph()->setFrequencies();
      }

      bool origTraceSetting = manager->trace();

      regex = comp()->getOptions()->getOptsToTrace();
      if (regex && TR::SimpleRegex::match(regex, optIndex))
         manager->setTrace(true);

      if (doThisOptimizationIfEnabled)
         manager->setPerformOnlyOnEnabledBlocks(true);

      // check if method exceeds loop or basic block threshold
      if (manager->getRequiresStructure() && comp()->getFlowGraph()->getStructure())
      {
         if (checkNumberOfLoopsAndBasicBlocks(comp(), comp()->getFlowGraph()->getStructure()))
         {
            if (comp()->getOption(TR_ProcessHugeMethods))
            {
               dumpOptDetails(comp(), "Method is normally too large (%d blocks and %d loops) but limits overridden\n", _numBasicBlocksInMethod, _numLoopsInMethod);
            }
            else
            {
               if (comp()->getOption(TR_MimicInterpreterFrameShape))
               {
                  comp()->failCompilation<TR::ExcessiveComplexity>("complex method under MimicInterpreterFrameShape");
               }
               else
               {
                  comp()->failCompilation<TR::ExcessiveComplexity>("Method is too large");
               }
            }
         }
      }

      comp()->reportOptimizationPhase(optNum);
      breakForTesting(optNum);
      if (!doThisOptimizationIfEnabled ||
          manager->getRequestedBlocks()->find(toBlock(comp()->getFlowGraph()->getStart())) ||
          manager->getRequestedBlocks()->find(toBlock(comp()->getFlowGraph()->getEnd())))
      {

         TR_ASSERT((justSetLastRun || !manager->getLastRun()), "%s shouldn't be run after LastRun was set\n", manager->name());

         manager->setRequested(false);

         comp()->recordBegunOpt();
         if (comp()->getOption(TR_TraceLastOpt) && comp()->getOptIndex() == comp()->getOptions()->getLastOptIndex())
         {
            comp()->getOptions()->enableTracing(optNum);
            manager->setTrace(true);
         }

         comp()->reportAnalysisPhase(PERFORMING_OPTIMIZATION);

         {
            TR::StackMemoryRegion stackMemoryRegion(*trMemory());
            opt->prePerform();
            actualCost += opt->perform();
            opt->postPerform();
         }

         comp()->reportAnalysisPhase(AFTER_OPTIMIZATION);
      }
      else if (canRunBlockByBlockOptimizations())
      {
         TR::StackMemoryRegion stackMemoryRegion(*trMemory());

         opt->prePerformOnBlocks();
         ListIterator<TR::Block> blockIt(manager->getRequestedBlocks());
         manager->setRequested(false);
         manager->setPerformOnlyOnEnabledBlocks(false);
         for (TR::Block *block = blockIt.getFirst(); block != NULL; block = blockIt.getNext())
         {
            // if (!comp()->getFlowGraph()->getRemovedNodes().find(block))
            if (!block->nodeIsRemoved())
            {
               block = block->startOfExtendedBlock();
               TR_ASSERT((justSetLastRun || !manager->getLastRun()), "opt %d shouldn't be run after LastRun was set for this optimization\n", optNum);
               actualCost += opt->performOnBlock(block);
            }
         }
         opt->postPerformOnBlocks();
      }

      delete opt;
      // we cannot easily invalidate during IL gen since we could be peeking and we cannot destroy our
      // caller's alias sets
      if (!isIlGenOpt())
         comp()->invalidateAliasRegion();
      breakForTesting(-optNum);

      if (comp()->compilationShouldBeInterrupted((TR_CallingContext)optNum))
      {
         comp()->failCompilation<TR::CompilationInterrupted>("interrupted between optimizations");
      }

      manager->setTrace(origTraceSetting);

      int32_t finalOptMsgIndex = self()->getOptMessageIndex();
      if ((finalOptMsgIndex != origOptMsgIndex) && !manager->getDoesNotRequireTreeDumps())
         comp()->reportOptimizationPhaseForSnap(optNum);

      if (comp()->getNodeCount() > unsigned(origNodeCount))
      {
         // If nodes were added, invalidate
         //
         setValueNumberInfo(NULL);
         if (!manager->getMaintainsUseDefInfo())
            setUseDefInfo(NULL);
      }

      if ((comp()->getSymRefCount() != origSymRefCount) /* || manager->getCanAddSymbolReference()*/)
      {
         setSymReferencesTable(NULL);
         // invalidate any alias sets so that they are rebuilt
         // by the next optimization that needs them
         //
         setAliasSetsAreValid(false);
      }

      if (comp()->getVisitCount() > HIGH_VISIT_COUNT)
      {
         comp()->resetVisitCounts(1);
         dumpOptDetails(comp(), "\nResetting visit counts for this method after %s\n", manager->name());
      }

      if (comp()->getFlowGraph()->getMightHaveUnreachableBlocks())
         comp()->getFlowGraph()->removeUnreachableBlocks();

#ifdef OPT_TIMING
      if (doTiming)
      {
         myTimer.stopTiming(comp());
         statOptTiming[optNum].update((double)myTimer.timeTaken() * 1000.0 / TR::Compiler->vm.getHighResClockResolution());
      }
#endif

#ifdef DEBUG
      if (manager->getDumpStructure() && debug("dumpStructure"))
      {
         traceMsg(comp(), "\nStructures:\n");
         getDebug()->print(comp()->getOutFile(), comp()->getFlowGraph()->getStructure(), 6);
      }

#endif

      if ((optIndex >= _firstDumpOptPhaseTrees && optIndex <= _lastDumpOptPhaseTrees) &&
          comp()->isOutermostMethod())
      {
         if (manager->getDoesNotRequireTreeDumps())
         {
            dumpOptDetails(comp(), "Trivial opt -- omitting lisitings\n");
         }
         else if (needTreeDump || (finalOptMsgIndex != origOptMsgIndex))
            comp()->dumpMethodTrees("Trees after ", manager->name(), getMethodSymbol());
         else if (finalOptMsgIndex == origOptMsgIndex)
         {
            dumpOptDetails(comp(), "No transformations done by this pass -- omitting listings\n");
            if (needStructureDump && comp()->getDebug() && comp()->getFlowGraph()->getStructure())
            {
               comp()->getDebug()->print(comp()->getOutFile(), comp()->getFlowGraph()->getStructure(), 6);
            }
         }
      }

#ifdef DEBUG
      if (debug("dumpGraphs") &&
          (_dumpGraphsIndex == -1 || _dumpGraphsIndex == optIndex))
         comp()->dumpMethodGraph(optIndex);
#endif

      manager->performChecks();

      static const bool enableCountTemps = feGetEnv("TR_EnableCountTemps") != NULL;
      if (enableCountTemps)
      {
         int32_t tempCount = 0;

         traceMsg(comp(), "Temps seen (if any): ");

         for (TR::TreeTop *tt = getMethodSymbol()->getFirstTreeTop(); tt; tt = tt->getNextTreeTop())
         {
            TR::Node *ttNode = tt->getNode();

            if (ttNode->getOpCodeValue() == TR::treetop)
            {
               ttNode = ttNode->getFirstChild();
            }

            if (ttNode->getOpCode().isStore() && ttNode->getOpCode().hasSymbolReference())
            {
               TR::SymbolReference *symRef = ttNode->getSymbolReference();

               if ((symRef->getSymbol()->getKind() == TR::Symbol::IsAutomatic) &&
                   symRef->isTemporary(comp()))
               {
                  ++tempCount;
                  traceMsg(comp(), "%s ", comp()->getDebug()->getName(ttNode->getSymbolReference()));
               }
            }
         }

         traceMsg(comp(), "\nNumber of temps seen = %d\n", tempCount);
      }

      if (comp()->getOption(TR_TraceOptDetails))
      {
         if (comp()->isOutermostMethod())
            traceMsg(comp(), "</optimization>\n\n");
      }
   }

   return actualCost;
}

void OMR::Optimizer::enableAllLocalOpts()
{
   setRequestOptimization(lateLocalGroup, true);
   setRequestOptimization(localCSE, true);
   setRequestOptimization(localValuePropagationGroup, true);
   setRequestOptimization(treeSimplification, true);
   setRequestOptimization(localDeadStoreElimination, true);
   setRequestOptimization(deadTreesElimination, true);
   setRequestOptimization(catchBlockRemoval, true);
   setRequestOptimization(compactNullChecks, true);
   setRequestOptimization(localReordering, true);
   setRequestOptimization(andSimplification, true);
   setRequestOptimization(redundantGotoElimination, true);
}

int32_t OMR::Optimizer::doStructuralAnalysis()
{

   // Only perform structural analysis if there may be loops in the method
   //
   // TEMPORARY HACK - always do structural analysis
   //
   TR_Structure *rootStructure = NULL;
   /////if (comp()->mayHaveLoops())
   {
      LexicalTimer t("StructuralAnalysis", comp()->phaseTimer());
      rootStructure = TR_RegionAnalysis::getRegions(comp());
      comp()->getFlowGraph()->setStructure(rootStructure);

      if (debug("dumpStructure"))
      {
         traceMsg(comp(), "\nStructures:\n");
         getDebug()->print(comp()->getOutFile(), rootStructure, 6);
      }
   }

   return 10;
}

int32_t OMR::Optimizer::changeContinueLoopsToNestedLoops()
{
   TR_RegionStructure *rootStructure = comp()->getFlowGraph()->getStructure()->asRegion();
   if (rootStructure && rootStructure->changeContinueLoopsToNestedLoops(rootStructure))
   {
      comp()->getFlowGraph()->setStructure(NULL);
      doStructuralAnalysis();
   }

   return 10;
}

bool OMR::Optimizer::prepareForNodeRemoval(TR::Node *node, bool deferInvalidatingUseDefInfo)
{
   int32_t index;

   TR_UseDefInfo *udInfo = getUseDefInfo();
   bool useDefInfoAreInvalid = false;
   if (udInfo)
   {
      index = node->getUseDefIndex();
      if (udInfo->isUseIndex(index))
      {
         // udInfo->setUseDefInfoToNull(index);
         udInfo->resetDefUseInfo();

         // If the node is both a use and a def we can't repair the info, since
         // it is a def to other uses that we don't know about (it's an unresolved
         // load, which acts like a call def node).
         //
         if (udInfo->isDefIndex(index))
         {
            if (!deferInvalidatingUseDefInfo)
               setUseDefInfo(NULL);
            useDefInfoAreInvalid = true;
         }
      }
      node->setUseDefIndex(0);
   }

   TR_ValueNumberInfo *vnInfo = getValueNumberInfo();
   if (vnInfo)
   {
      vnInfo->removeNodeInfo(node);
   }

   for (int32_t i = node->getNumChildren() - 1; i >= 0; i--)
   {
      TR::Node *child = node->getChild(i);
      if (child != NULL && child->getReferenceCount() == 1)
         if (prepareForNodeRemoval(child))
            useDefInfoAreInvalid = true;
   }
   return useDefInfoAreInvalid;
}

void OMR::Optimizer::getStaticFrequency(TR::Block *block, int32_t *currentWeight)
{
   if (comp()->getUsesBlockFrequencyInGRA())
      *currentWeight = block->getFrequency();
   else
      block->getStructureOf()->calculateFrequencyOfExecution(currentWeight);
}

TR_Hotness OMR::Optimizer::checkMaxHotnessOfInlinedMethods(TR::Compilation *comp)
{
   TR_Hotness strategy = comp->getMethodHotness();
#ifdef J9_PROJECT_SPECIFIC
   if (comp->getNumInlinedCallSites() > 0)
   {
      for (uint32_t i = 0; i < comp->getNumInlinedCallSites(); ++i)
      {
         TR_InlinedCallSite &ics = comp->getInlinedCallSite(i);
         TR_OpaqueMethodBlock *method = comp->fe()->getInlinedCallSiteMethod(&ics);
         if (TR::Compiler->mtd.isCompiledMethod(method))
         {
            TR_PersistentJittedBodyInfo *bodyInfo = TR::Recompilation::getJittedBodyInfoFromPC((void *)TR::Compiler->mtd.startPC(method));
            if (bodyInfo &&
                bodyInfo->getHotness() > strategy)
            {
               strategy = bodyInfo->getHotness();
            }
            else if (!bodyInfo && TR::Options::getCmdLineOptions()->allowRecompilation()) // don't do it for fixed level
            {
               strategy = scorching;
               break;
            }
         }
      }
   }
#endif
   return strategy;
}

bool OMR::Optimizer::checkNumberOfLoopsAndBasicBlocks(TR::Compilation *comp, TR_Structure *rootStructure)
{
   TR::CFGNode *node;
   _numBasicBlocksInMethod = 0;
   for (node = comp->getFlowGraph()->getFirstNode(); node; node = node->getNext())
   {
      _numBasicBlocksInMethod++;
   }

   // dumpOptDetails(comp(), "Number of nodes in the cfg = %d\n", _numBasicBlocksInMethod);

   _numLoopsInMethod = 0;
   countNumberOfLoops(rootStructure);
   // dumpOptDetails(comp(), "Number of loops in the cfg = %d\n", _numLoopsInMethod);

   int32_t highBasicBlockCount = HIGH_BASIC_BLOCK_COUNT;
   int32_t highLoopCount = HIGH_LOOP_COUNT;
   // set loop count thershold to a higher value for now
   // TODO: find a better way to fix this by creating a check
   // about _disableLoopOptsThatCanCreateLoops
   if (comp->getMethodHotness() >= veryHot)
      highLoopCount = VERY_HOT_HIGH_LOOP_COUNT;
   if (comp->isOptServer())
   {
      highBasicBlockCount = highBasicBlockCount * 2;
      highLoopCount = highLoopCount * 2;
   }

   if ((_numBasicBlocksInMethod >= highBasicBlockCount) ||
       (_numLoopsInMethod >= highLoopCount))
   {
      return true;
   }
   return false;
}

void OMR::Optimizer::countNumberOfLoops(TR_Structure *rootStructure)
{
   TR_RegionStructure *regionStructure = rootStructure->asRegion();
   if (regionStructure)
   {
      if (regionStructure->isNaturalLoop())
         _numLoopsInMethod++;
      TR_StructureSubGraphNode *node;
      TR_RegionStructure::Cursor si(*regionStructure);
      for (node = si.getFirst(); node; node = si.getNext())
         countNumberOfLoops(node->getStructure());
   }
}

bool OMR::Optimizer::areNodesEquivalent(TR::Node *node1, TR::Node *node2, TR::Compilation *_comp, bool allowBCDSignPromotion)
{
   // WCodeLinkageFixup runs a version of LocalCSE that is not owned by
   // an optimizer, so it has to pass in a TR_Compilation

   if (node1 == node2)
      return true;

   if (!(node1->getOpCodeValue() == node2->getOpCodeValue()))
      return false;

   TR::ILOpCode &opCode1 = node1->getOpCode();
   if (opCode1.isSwitch() == 0)
   {
      if (opCode1.hasSymbolReference())
      {
         if (node1->getSymbolReference()->getReferenceNumber() != node2->getSymbolReference()->getReferenceNumber())
         {
            return false;
         }
         else if ((opCode1.isCall() && !node1->isPureCall()) ||
                  opCode1.isStore() ||
                  opCode1.getOpCodeValue() == TR::New ||
                  opCode1.getOpCodeValue() == TR::newarray ||
                  opCode1.getOpCodeValue() == TR::anewarray ||
                  opCode1.getOpCodeValue() == TR::multianewarray ||
                  opCode1.getOpCodeValue() == TR::monent ||
                  opCode1.getOpCodeValue() == TR::monexit)
         {
            if (!(node1 == node2))
               return false;
         }
      }
      else if (opCode1.isBranch())
      {
         if (!(node1->getBranchDestination()->getNode() == node2->getBranchDestination()->getNode()))
            return false;
      }

#ifdef J9_PROJECT_SPECIFIC
      if (node1->getOpCode().isSetSignOnNode() && node1->getSetSign() != node2->getSetSign())
         return false;
#endif

      if (opCode1.isLoadConst())
      {
         switch (node1->getDataType())
         {
         case TR::Int8:
            if (node1->getByte() != node2->getByte())
               return false;
            break;
         case TR::Int16:
            if (node1->getShortInt() != node2->getShortInt())
               return false;
            break;
         case TR::Int32:
            if (node1->getInt() != node2->getInt())
               return false;
            break;
         case TR::Int64:
            if (node1->getLongInt() != node2->getLongInt())
               return false;
            break;
         case TR::Float:
            if (node1->getFloatBits() != node2->getFloatBits())
               return false;
            break;
         case TR::Double:
            if (node1->getDoubleBits() != node2->getDoubleBits())
               return false;
            break;
         case TR::Address:
            if (node1->getAddress() != node2->getAddress())
               return false;
            break;
         case TR::VectorInt64:
         case TR::VectorInt32:
         case TR::VectorInt16:
         case TR::VectorInt8:
         case TR::VectorDouble:
            if (node1->getLiteralPoolOffset() != node2->getLiteralPoolOffset())
               return false;
            break;
#ifdef J9_PROJECT_SPECIFIC
         case TR::Aggregate:
            if (!areBCDAggrConstantNodesEquivalent(node1, node2, _comp))
            {
               return false;
            }
#endif
            break;
         default:
         {
#ifdef J9_PROJECT_SPECIFIC
            if (node1->getDataType().isBCD())
            {
               if (!areBCDAggrConstantNodesEquivalent(node1, node2, _comp))
                  return false;
            }
#endif
         }
         }
      }
      else if (opCode1.isArrayLength())
      {
         if (node1->getArrayStride() != node2->getArrayStride())
            return false;
      }
#ifdef J9_PROJECT_SPECIFIC
      else if (node1->getType().isBCD())
      {
         if (node1->isDecimalSizeAndShapeEquivalent(node2))
         {
            // LocalAnalysis temporarily changes store opcodes to load opcodes to enable matching up loads/stores
            // However since sign state is not tracked (and is not relevant) for stores this causes the equivalence
            // test to unnecessarily fail. The isBCDStoreTemporarilyALoad flag allow skipping of the sign state compare
            // for these cases.
            if (!(node1->getOpCode().isLoadVar() && node1->isBCDStoreTemporarilyALoad()) &&
                !(node2->getOpCode().isLoadVar() && node2->isBCDStoreTemporarilyALoad()) &&
                !node1->isSignStateEquivalent(node2))
            {
               if (allowBCDSignPromotion && node1->isSignStateAnImprovementOver(node2))
               {
                  if (_comp->cg()->traceBCDCodeGen())
                     traceMsg(_comp, "y^y : found sign state mismatch node1 %s (%p), node2 %s (%p) but node1 improves sign state over node2\n",
                              node1->getOpCode().getName(), node1, node2->getOpCode().getName(), node2);
                  return true;
               }
               else
               {
                  if (_comp->cg()->traceBCDCodeGen())
                     traceMsg(_comp, "x^x : found sign state mismatch node1 %s (%p), node2 %s (%p)\n",
                              node1->getOpCode().getName(), node1, node2->getOpCode().getName(), node2);
                  return false;
               }
            }
         }
         else
         {
            return false;
         }
      }
      else if (opCode1.isConversionWithFraction() &&
               node1->getDecimalFraction() != node2->getDecimalFraction())
      {
         return false;
      }
      else if (node1->chkOpsCastedToBCD() &&
               node1->castedToBCD() != node2->castedToBCD())
      {
         return false;
      }
      else if (opCode1.getOpCodeValue() == TR::loadaddr &&
               (node1->getSymbolReference()->isTempVariableSizeSymRef() && node2->getSymbolReference()->isTempVariableSizeSymRef()) &&
               (node1->getDecimalPrecision() != node2->getDecimalPrecision()))
      {
         return false;
      }
#endif
      else if (opCode1.isArrayRef())
      {
         // for some reason this tests hasPinningArrayPointer only when the node also is true on _flags.testAny(internalPointer)
         bool haveIPs = node1->isInternalPointer() && node2->isInternalPointer();
         bool haveNoIPs = !node1->isInternalPointer() && !node2->isInternalPointer();
         TR::AutomaticSymbol *pinning1 = node1->getOpCode().hasPinningArrayPointer() ? node1->getPinningArrayPointer() : NULL;
         TR::AutomaticSymbol *pinning2 = node2->getOpCode().hasPinningArrayPointer() ? node2->getPinningArrayPointer() : NULL;
         if ((haveIPs && (pinning1 == pinning2)) || haveNoIPs)
            return true;
         else
            return false;
      }
      else if (opCode1.getOpCodeValue() == TR::PassThrough)
      {
         return false;
      }
      else if (opCode1.isLoadReg())
      {
         if (!node2->getOpCode().isLoadReg())
         {
            return false;
         }

         if (node1->getGlobalRegisterNumber() != node2->getGlobalRegisterNumber())
         {
            return false;
         }
      } // IvanB
   }
   else
   {
      if (!(areNodesEquivalent(node1->getFirstChild(), node2->getFirstChild(), _comp)))
         return false;

      if (!(node1->getSecondChild()->getBranchDestination()->getNode() == node2->getSecondChild()->getBranchDestination()->getNode()))
         return false;

      if (opCode1.getOpCodeValue() == TR::lookup)
      {
         for (int i = node1->getCaseIndexUpperBound() - 1; i > 1; i--)
         {
            if (!(node1->getChild(i)->getBranchDestination()->getNode() == node2->getChild(i)->getBranchDestination()->getNode()))
               return false;
         }
      }
      else if (opCode1.getOpCodeValue() == TR::table)
      {
         for (int i = node1->getCaseIndexUpperBound() - 1; i > 1; i--)
         {
            if (!(node1->getChild(i)->getBranchDestination()->getNode() == node2->getChild(i)->getBranchDestination()->getNode()))
               return false;
         }
      }
   }

   return true;
}

#ifdef J9_PROJECT_SPECIFIC
bool OMR::Optimizer::areBCDAggrConstantNodesEquivalent(TR::Node *node1, TR::Node *node2, TR::Compilation *_comp)
{
   size_t size1 = (node1->getDataType().isBCD()) ? node1->getDecimalPrecision() : 0;
   size_t size2 = (node2->getDataType().isBCD()) ? node2->getDecimalPrecision() : 0;

   if (size1 != size2)
   {
      return false;
   }
   if (node1->getNumChildren() == 1 && node2->getNumChildren() == 1 && // if neither is a delayed literal,
       node1->getLiteralPoolOffset() != node2->getLiteralPoolOffset()) // compare their offsets in the literal pool.
   {
      return false;
   }
   return true;
}
#endif

bool OMR::Optimizer::areSyntacticallyEquivalent(TR::Node *node1, TR::Node *node2, vcount_t visitCount)
{
   if (node1->getVisitCount() == visitCount)
   {
      if (node2->getVisitCount() == visitCount)
         return true;
      else
         return false;
   }

   if (node2->getVisitCount() == visitCount)
   {
      if (node1->getVisitCount() == visitCount)
         return true;
      else
         return false;
   }

   bool equivalent = true;
   if (!areNodesEquivalent(node1, node2))
      equivalent = false;

   if (node1->getNumChildren() != node2->getNumChildren())
      equivalent = false;

   if (equivalent)
   {
      int32_t numChildren = node1->getNumChildren();
      int32_t i;
      for (i = numChildren - 1; i >= 0; i--)
      {
         TR::Node *child1 = node1->getChild(i);
         TR::Node *child2 = node2->getChild(i);

         if (!areSyntacticallyEquivalent(child1, child2, visitCount))
         {
            equivalent = false;
            break;
         }
      }
   }

   return equivalent;
}

/**
 * Build the table of corresponding symbol references for use by optimizations.
 * This table allows a fast determination of whether two symbol references
 * represent the same symbol.
 */
int32_t *OMR::Optimizer::getSymReferencesTable()
{
   if (_symReferencesTable == NULL)
   {
      int32_t symRefCount = comp()->getSymRefCount();
      _symReferencesTable = (int32_t *)trMemory()->allocateStackMemory(symRefCount * sizeof(int32_t));
      memset(_symReferencesTable, 0, symRefCount * sizeof(int32_t));
      TR::SymbolReferenceTable *symRefTab = comp()->getSymRefTab();
      for (int32_t symRefNumber = 0; symRefNumber < symRefCount; symRefNumber++)
      {
         bool newSymbol = true;
         if (symRefNumber >= comp()->getSymRefTab()->getIndexOfFirstSymRef())
         {
            TR::SymbolReference *symRef = symRefTab->getSymRef(symRefNumber);
            TR::Symbol *symbol = symRef ? symRef->getSymbol() : 0;
            if (symbol)
            {
               for (int32_t i = comp()->getSymRefTab()->getIndexOfFirstSymRef(); i < symRefNumber; ++i)
               {
                  if (_symReferencesTable[i] == i)
                  {
                     TR::SymbolReference *otherSymRef = symRefTab->getSymRef(i);
                     TR::Symbol *otherSymbol = otherSymRef ? otherSymRef->getSymbol() : 0;
                     if (otherSymbol && symbol == otherSymbol && symRef->getOffset() == otherSymRef->getOffset())
                     {
                        newSymbol = false;
                        _symReferencesTable[symRefNumber] = i;
                        break;
                     }
                  }
               }
            }
         }

         if (newSymbol)
            _symReferencesTable[symRefNumber] = symRefNumber;
      }
   }
   return _symReferencesTable;
}

#ifdef DEBUG
void OMR::Optimizer::doStructureChecks()
{
   TR::CFG *cfg = getMethodSymbol()->getFlowGraph();
   if (cfg)
   {
      TR_Structure *rootStructure = cfg->getStructure();
      if (rootStructure)
      {
         TR::StackMemoryRegion stackMemoryRegion(*trMemory());

         // Allocate bit vector of block numbers that have been seen
         //
         TR_BitVector blockNumbers(cfg->getNextNodeNumber(), comp()->trMemory(), stackAlloc);
         rootStructure->checkStructure(&blockNumbers);
      }
   }
}
#endif

bool OMR::Optimizer::getLastRun(OMR::Optimizations opt)
{
   if (!_opts[opt])
      return false;
   return _opts[opt]->getLastRun();
}

void OMR::Optimizer::setRequestOptimization(OMR::Optimizations opt, bool value, TR::Block *block)
{
   if (_opts[opt])
      _opts[opt]->setRequested(value, block);
}

void OMR::Optimizer::setAliasSetsAreValid(bool b, bool setForWCode)
{
   if (_aliasSetsAreValid && !b)
      dumpOptDetails(comp(), "     (Invalidating alias info)\n");

   _aliasSetsAreValid = b;
}

const OptimizationStrategy *OMR::Optimizer::_mockStrategy = NULL;

const OptimizationStrategy *
OMR::Optimizer::optimizationStrategy(TR::Compilation *c)
{
   // Mock strategies are used for testing, and override
   // the compilation strategy.
   if (NULL != OMR::Optimizer::_mockStrategy)
   {
      traceMsg(c, "Using mock optimization strategy %p\n", OMR::Optimizer::_mockStrategy);
      return OMR::Optimizer::_mockStrategy;
   }

   TR_Hotness strategy = c->getMethodHotness();
   TR_ASSERT(strategy <= lastOMRStrategy, "Invalid optimization strategy");

   // Downgrade strategy rather than crashing in prod.
   if (strategy > lastOMRStrategy)
      strategy = lastOMRStrategy;

   return omrCompilationStrategies[strategy];
}

ValueNumberInfoBuildType
OMR::Optimizer::valueNumberInfoBuildType()
{
   return PrePartitionVN;
}

TR::Optimizer *OMR::Optimizer::self()
{
   return (static_cast<TR::Optimizer *>(this));
}

OMR_InlinerPolicy *OMR::Optimizer::getInlinerPolicy()
{
   return new (comp()->allocator()) OMR_InlinerPolicy(comp());
}

OMR_InlinerUtil *OMR::Optimizer::getInlinerUtil()
{
   return new (comp()->allocator()) OMR_InlinerUtil(comp());
}
