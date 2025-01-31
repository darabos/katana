#include <boost/filesystem.hpp>

#include "TestTypedPropertyGraph.h"
#include "katana/GraphTopology.h"
#include "katana/Iterators.h"
#include "katana/Logging.h"
#include "katana/PropertyGraph.h"
#include "katana/Result.h"
#include "katana/SharedMemSys.h"
#include "katana/analytics/Utils.h"
#include "llvm/Support/CommandLine.h"
#include "stdio.h"
#include "storage-format-version.h"
#include "tsuba/RDG.h"

namespace cll = llvm::cl;
namespace fs = boost::filesystem;

/*
 * Tests to validate optional topology storage added in storage_format_version=3
 * Input can be any rdg with storage_format_version < 3
 */

static cll::opt<std::string> ldbc_003InputFile(
    cll::Positional, cll::desc("<ldbc_003 input file>"), cll::Required);

template <typename View>
void
verify_view(View generated_view, View loaded_view) {
  KATANA_LOG_ASSERT(generated_view.num_edges() == loaded_view.num_edges());
  KATANA_LOG_ASSERT(generated_view.num_nodes() == loaded_view.num_nodes());

  auto beg_edge = katana::make_zip_iterator(
      generated_view.all_edges().begin(), loaded_view.all_edges().begin());
  auto end_edge = katana::make_zip_iterator(
      generated_view.all_edges().end(), loaded_view.all_edges().end());

  for (auto i = beg_edge; i != end_edge; i++) {
    KATANA_LOG_ASSERT(std::get<0>(*i) == std::get<1>(*i));
  }

  auto beg_node = katana::make_zip_iterator(
      generated_view.all_nodes().begin(), loaded_view.all_nodes().begin());
  auto end_node = katana::make_zip_iterator(
      generated_view.all_nodes().end(), loaded_view.all_nodes().end());

  for (auto i = beg_node; i != end_node; i++) {
    KATANA_LOG_ASSERT(std::get<0>(*i) == std::get<1>(*i));
  }
}

void
TestOptionalTopologyStorageEdgeShuffleTopology() {
  KATANA_LOG_WARN("***** Testing EdgeShuffleTopology *****");

  katana::PropertyGraph pg = LoadGraph(ldbc_003InputFile);

  // Build a EdgeSortedByDestID view, which uses GraphTopology EdgeShuffleTopology in the background
  using SortedGraphView = katana::PropertyGraphViews::EdgesSortedByDestID;

  SortedGraphView generated_sorted_view = pg.BuildView<SortedGraphView>();
  // TODO: ensure this view was generated, not loaded
  // generated_sorted_view.Print();

  std::string g2_rdg_file = StoreGraph(&pg);
  katana::PropertyGraph pg2 = LoadGraph(g2_rdg_file);

  SortedGraphView loaded_sorted_view = pg2.BuildView<SortedGraphView>();

  //TODO: emcginnis need some way to verify we loaded this view, vs just generating it again

  verify_view(generated_sorted_view, loaded_sorted_view);
}

void
TestOptionalTopologyStorageShuffleTopology() {
  KATANA_LOG_WARN("***** Testing ShuffleTopology *****");

  katana::PropertyGraph pg = LoadGraph(ldbc_003InputFile);

  // Build a NodesSortedByDegreeEdgesSortedByDestID view, which uses GraphTopology ShuffleTopology in the background
  using SortedGraphView =
      katana::PropertyGraphViews::NodesSortedByDegreeEdgesSortedByDestID;

  SortedGraphView generated_sorted_view = pg.BuildView<SortedGraphView>();
  // TODO: ensure this view was generated, not loaded
  // generated_sorted_view.Print();

  std::string g2_rdg_file = StoreGraph(&pg);
  katana::PropertyGraph pg2 = LoadGraph(g2_rdg_file);

  SortedGraphView loaded_sorted_view = pg2.BuildView<SortedGraphView>();

  //TODO: emcginnis need some way to verify we loaded this view, vs just generating it again

  verify_view(generated_sorted_view, loaded_sorted_view);
}

void
TestOptionalTopologyStorageEdgeTypeAwareTopology() {
  KATANA_LOG_WARN("***** Testing EdgeTypeAware Topology *****");

  katana::PropertyGraph pg = LoadGraph(ldbc_003InputFile);

  // Build a EdgeTypeAwareBiDir view, which uses GraphTopology EdgeTypeAwareTopology in the background
  using SortedGraphView = katana::PropertyGraphViews::EdgeTypeAwareBiDir;

  SortedGraphView generated_sorted_view = pg.BuildView<SortedGraphView>();
  // generated_sorted_view.Print();

  std::string g2_rdg_file = StoreGraph(&pg);
  katana::PropertyGraph pg2 = LoadGraph(g2_rdg_file);

  SortedGraphView loaded_sorted_view = pg2.BuildView<SortedGraphView>();

  //TODO: emcginnis need some way to verify we loaded this view, vs just generating it again

  verify_view(generated_sorted_view, loaded_sorted_view);
}

int
main(int argc, char** argv) {
  katana::SharedMemSys sys;
  cll::ParseCommandLineOptions(argc, argv);

  TestOptionalTopologyStorageEdgeShuffleTopology();
  TestOptionalTopologyStorageShuffleTopology();
  TestOptionalTopologyStorageEdgeTypeAwareTopology();
  return 0;
}
