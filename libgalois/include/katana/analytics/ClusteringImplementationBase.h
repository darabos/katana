/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#ifndef KATANA_LIBGALOIS_KATANA_ANALYTICS_CLUSTERINGIMPLEMENTATIONBASE_H_
#define KATANA_LIBGALOIS_KATANA_ANALYTICS_CLUSTERINGIMPLEMENTATIONBASE_H_

#include <fstream>
#include <iostream>
#include <random>

#include "katana/AtomicHelpers.h"
#include "katana/Galois.h"
#include "katana/NUMAArray.h"
#include "katana/analytics/Utils.h"

namespace katana::analytics {

// Maintain community information
template <typename EdgeWeightType>
struct CommunityType {
  std::atomic<uint64_t> size;
  std::atomic<EdgeWeightType> degree_wt;
  EdgeWeightType internal_edge_wt;
};

template <typename EdgeWeightType>
struct LeidenCommunityType {
  std::atomic<uint64_t> size;
  std::atomic<EdgeWeightType> degree_wt;
  std::atomic<uint64_t> node_wt;
  EdgeWeightType internal_edge_wt;
  uint64_t num_sub_communities;
};

struct PreviousCommunityID : public katana::PODProperty<uint64_t> {};
struct CurrentCommunityID : public katana::PODProperty<uint64_t> {};

template <typename EdgeWeightType>
using DegreeWeight = katana::PODProperty<EdgeWeightType>;

template <typename EdgeWeightType>
using EdgeWeight = katana::PODProperty<EdgeWeightType>;

/* Leiden specific properties */
struct CurrentSubCommunityID : public katana::PODProperty<uint64_t> {};
struct NodeWeight : public katana::PODProperty<uint64_t> {};

template <typename _Graph, typename _EdgeType, typename _CommunityType>
struct ClusteringImplementationBase {
  using Graph = _Graph;
  using GNode = typename Graph::Node;
  using EdgeTy = _EdgeType;
  using CommunityType = _CommunityType;

  constexpr static const uint64_t UNASSIGNED =
      std::numeric_limits<uint64_t>::max();
  constexpr static const double INFINITY_DOUBLE =
      std::numeric_limits<double>::max() / 4;

  using CommunityArray = katana::NUMAArray<CommunityType>;

  /**
   * Algorithm to find the best cluster for the node
   * to move to among its neighbors in the graph and moves.
   *
   * It updates the mapping of neighboring nodes clusters
   * in cluster_local_map, total unique cluster edge weights
   * in counter as well as total weight of self edges in self_loop_wt.
   */
  template <typename EdgeWeightType>
  static void FindNeighboringClusters(
      const Graph& graph, GNode& n,
      std::map<uint64_t, uint64_t>& cluster_local_map,
      std::vector<EdgeTy>& counter, EdgeTy& self_loop_wt) {
    uint64_t num_unique_clusters = 0;

    // Add the node's current cluster to be considered
    // for movement as well
    cluster_local_map[graph.template GetData<CurrentCommunityID>(n)] =
        0;                 // Add n's current cluster
    counter.push_back(0);  // Initialize the counter to zero (no edges incident
                           // yet)
    num_unique_clusters++;

    // Assuming we have grabbed lock on all the neighbors
    for (auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
      auto dst = graph.GetEdgeDest(ii);
      auto edge_wt = graph.template GetEdgeData<EdgeWeight<EdgeWeightType>>(
          ii);  // Self loop weights is recorded
      if (*dst == n) {
        self_loop_wt += edge_wt;  // Self loop weights is recorded
      }
      auto stored_already =
          cluster_local_map.find(graph.template GetData<CurrentCommunityID>(
              dst));  // Check if it already exists
      if (stored_already != cluster_local_map.end()) {
        counter[stored_already->second] += edge_wt;
      } else {
        cluster_local_map[graph.template GetData<CurrentCommunityID>(dst)] =
            num_unique_clusters;
        counter.push_back(edge_wt);
        num_unique_clusters++;
      }
    }  // End edge loop
    return;
  }

  /**
   * Enables the filtering optimization to remove the
   * node with out-degree 0 (isolated) and 1 before the clustering
   * algorithm begins.
   */
  static uint64_t VertexFollowing(Graph* graph) {
    using GNode = typename Graph::Node;
    // Initialize each node to its own cluster
    katana::do_all(katana::iterate(*graph), [&](GNode n) {
      graph->template GetData<CurrentCommunityID>(n) = n;
    });

    // Remove isolated and degree-one nodes
    katana::GAccumulator<uint64_t> isolated_nodes;
    katana::do_all(katana::iterate(*graph), [&](GNode n) {
      auto& n_data_curr_comm_id =
          graph->template GetData<CurrentCommunityID>(n);
      uint64_t degree = std::distance(graph->edge_begin(n), graph->edge_end(n));
      if (degree == 0) {
        isolated_nodes += 1;
        n_data_curr_comm_id = UNASSIGNED;
      } else {
        if (degree == 1) {
          // Check if the destination has degree greater than one
          auto dst = graph->GetEdgeDest(graph->edge_end(n) - 1);
          uint64_t dst_degree =
              std::distance(graph->edge_begin(*dst), graph->edge_end(*dst));
          if ((dst_degree > 1 || (n > *dst))) {
            isolated_nodes += 1;
            n_data_curr_comm_id =
                graph->template GetData<CurrentCommunityID>(dst);
          }
        }
      }
    });
    // The number of isolated nodes that can be removed
    return isolated_nodes.reduce();
  }

  /**
   * Sums up the degree weight for all
   * the unique clusters.
   */
  template <typename EdgeWeightType>
  static void SumVertexDegreeWeight(Graph* graph, CommunityArray& c_info) {
    katana::do_all(katana::iterate(*graph), [&](GNode n) {
      EdgeTy total_weight = 0;
      auto& n_degree_wt =
          graph->template GetData<DegreeWeight<EdgeWeightType>>(n);
      for (auto ii = graph->edge_begin(n); ii != graph->edge_end(n); ++ii) {
        total_weight +=
            graph->template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
      }
      n_degree_wt = total_weight;
      c_info[n].degree_wt = total_weight;
      c_info[n].size = 1;
    });
    return;
  }

  /**
   * Computes the constant term 1/(2 * total internal edge weight)
   * of the current coarsened graph.
   */
  template <typename EdgeWeightType>
  static double CalConstantForSecondTerm(const Graph& graph) {
    //Using double to avoid overflow
    katana::GAccumulator<double> local_weight;
    katana::do_all(katana::iterate(graph), [&graph, &local_weight](GNode n) {
      local_weight += graph.template GetData<DegreeWeight<EdgeWeightType>>(n);
    });
    //This is twice since graph is symmetric
    double total_edge_weight_twice = local_weight.reduce();
    return 1 / total_edge_weight_twice;
  }

  /**
   * Computes the constant term 1/(2 * total internal edge weight)
   * of the current coarsened graph. Takes the optional NUMAArray
   * with edge weight. To be used if edge weight is missing in the
   * property graph.
   */
  template <typename EdgeWeightType>
  static double CalConstantForSecondTerm(
      const Graph& graph,
      katana::NUMAArray<EdgeWeightType>& degree_weight_array) {
    // Using double to avoid overflow
    katana::GAccumulator<double> local_weight;
    katana::do_all(katana::iterate(graph), [&](GNode n) {
      local_weight += degree_weight_array[n];
    });
    // This is twice since graph is symmetric
    double total_edge_weight_twice = local_weight.reduce();
    return 1 / total_edge_weight_twice;
  }

  /**
   * Computes the modularity gain of the current cluster assignment
   * without swapping the cluster assignment.
   */
  static uint64_t MaxModularityWithoutSwaps(
      std::map<uint64_t, uint64_t>& cluster_local_map,
      std::vector<EdgeTy>& counter, uint64_t self_loop_wt,
      CommunityArray& c_info, EdgeTy degree_wt, uint64_t sc, double constant) {
    uint64_t max_index = sc;  // Assign the intial value as self community
    double cur_gain = 0;
    double max_gain = 0;
    double eix = counter[0] - self_loop_wt;
    double ax = c_info[sc].degree_wt - degree_wt;
    double eiy = 0;
    double ay = 0;

    auto stored_already = cluster_local_map.begin();
    do {
      if (sc != stored_already->first) {
        ay = c_info[stored_already->first].degree_wt;  // Degree wt of cluster y

        if (ay < (ax + degree_wt)) {
          stored_already++;
          continue;
        } else if (ay == (ax + degree_wt) && stored_already->first > sc) {
          stored_already++;
          continue;
        }

        eiy = counter[stored_already
                          ->second];  // Total edges incident on cluster y
        cur_gain = 2 * constant * (eiy - eix) +
                   2 * degree_wt * ((ax - ay) * constant * constant);

        if ((cur_gain > max_gain) ||
            ((cur_gain == max_gain) && (cur_gain != 0) &&
             (stored_already->first < max_index))) {
          max_gain = cur_gain;
          max_index = stored_already->first;
        }
      }
      stored_already++;  // Explore next cluster
    } while (stored_already != cluster_local_map.end());

    if ((c_info[max_index].size == 1 && c_info[sc].size == 1 &&
         max_index > sc)) {
      max_index = sc;
    }

    KATANA_LOG_DEBUG_ASSERT(max_gain >= 0);
    return max_index;
  }

  /**
   * Computes the modularity gain of the current cluster assignment.
   */
  template <typename EdgeWeightType>
  static double CalModularity(
      const Graph& graph, CommunityArray& c_info, double& e_xx, double& a2_x,
      double& constant_for_second_term) {
    /* Variables needed for Modularity calculation */
    double mod = -1;

    katana::NUMAArray<EdgeTy> cluster_wt_internal;

    /*** Initialization ***/
    cluster_wt_internal.allocateBlocked(graph.num_nodes());

    /* Calculate the overall modularity */
    katana::GAccumulator<double> acc_e_xx;
    katana::GAccumulator<double> acc_a2_x;

    katana::do_all(
        katana::iterate(graph), [&](GNode n) { cluster_wt_internal[n] = 0; });

    katana::do_all(katana::iterate(graph), [&](GNode n) {
      auto n_data_current_comm_id =
          graph.template GetData<CurrentCommunityID>(n);
      for (auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
        if (graph.template GetData<CurrentCommunityID>(graph.GetEdgeDest(ii)) ==
            n_data_current_comm_id) {
          cluster_wt_internal[n] +=
              graph.template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
        }
      }
    });

    katana::do_all(katana::iterate(graph), [&](GNode n) {
      acc_e_xx += cluster_wt_internal[n];
      acc_a2_x +=
          (double)(c_info[n].degree_wt) *
          ((double)(c_info[n].degree_wt) * (double)constant_for_second_term);
    });

    e_xx = acc_e_xx.reduce();
    a2_x = acc_a2_x.reduce();

    mod = e_xx * (double)constant_for_second_term -
          a2_x * (double)constant_for_second_term;
    return mod;
  }

  template <typename EdgeWeightType, typename NodePropType>
  static void SumClusterWeight(
      Graph& graph, CommunityArray& c_info,
      katana::NUMAArray<EdgeWeightType>& degree_weight_array) {
    using GNode = typename Graph::Node;

    katana::do_all(katana::iterate(graph), [&](GNode n) {
      EdgeTy total_weight = 0;
      for (auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
        total_weight +=
            graph.template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
      }
      degree_weight_array[n] = total_weight;
      c_info[n].degree_wt = 0;
    });

    katana::do_all(katana::iterate(graph), [&](GNode n) {
      auto& n_data_comm_id = graph.template GetData<NodePropType>(n);
      if (n_data_comm_id != UNASSIGNED)
        katana::atomicAdd(
            c_info[n_data_comm_id].degree_wt, degree_weight_array[n]);
    });
  }

  /**
 * Computes the final modularity using prev cluster
 * assignments.
 */
  template <typename EdgeWeightType, typename CommunityIDType>
  static double CalModularityFinal(Graph& graph) {
    CommunityArray c_info;    // Community info
    CommunityArray c_update;  // Used for updating community

    /* Variables needed for Modularity calculation */
    double constant_for_second_term;
    double mod = -1;

    katana::NUMAArray<EdgeTy> cluster_wt_internal;

    /*** Initialization ***/
    c_info.allocateBlocked(graph.num_nodes());
    c_update.allocateBlocked(graph.num_nodes());
    cluster_wt_internal.allocateBlocked(graph.num_nodes());

    katana::NUMAArray<EdgeWeightType> degree_weight_array;
    degree_weight_array.allocateBlocked(graph.num_nodes());

    /* Calculate the weighted degree sum for each vertex */
    SumClusterWeight<EdgeWeightType, CommunityIDType>(
        graph, c_info, degree_weight_array);

    /* Compute the total weight (2m) and 1/2m terms */
    constant_for_second_term =
        CalConstantForSecondTerm<EdgeWeightType>(graph, degree_weight_array);

    /* Calculate the overall modularity */
    double e_xx = 0;
    katana::GAccumulator<double> acc_e_xx;
    double a2_x = 0;
    katana::GAccumulator<double> acc_a2_x;

    katana::do_all(
        katana::iterate(graph), [&](GNode n) { cluster_wt_internal[n] = 0; });

    katana::do_all(katana::iterate(graph), [&](GNode n) {
      auto n_data_current_comm = graph.template GetData<CommunityIDType>(n);
      for (auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
        if (graph.template GetData<CommunityIDType>(graph.GetEdgeDest(ii)) ==
            n_data_current_comm) {
          cluster_wt_internal[n] +=
              graph.template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
        }
      }
    });

    katana::do_all(katana::iterate(graph), [&](GNode n) {
      acc_e_xx += cluster_wt_internal[n];
      acc_a2_x +=
          (double)(c_info[n].degree_wt) *
          ((double)(c_info[n].degree_wt) * (double)constant_for_second_term);
    });

    e_xx = acc_e_xx.reduce();
    a2_x = acc_a2_x.reduce();

    mod = e_xx * (double)constant_for_second_term -
          a2_x * (double)constant_for_second_term;
    return mod;
  }

  /**
 * Renumbers the cluster to contiguous cluster ids
 * to fill the holes in the cluster id assignments.
 */
  template <typename CommunityIDType>
  static uint64_t RenumberClustersContiguously(Graph* graph) {
    std::map<uint64_t, uint64_t> cluster_local_map;
    uint64_t num_unique_clusters = 0;

    for (GNode n = 0; n < graph->num_nodes(); ++n) {
      auto& n_data_curr_comm_id = graph->template GetData<CommunityIDType>(n);
      if (n_data_curr_comm_id != UNASSIGNED) {
        KATANA_LOG_DEBUG_ASSERT(n_data_curr_comm_id < graph->num_nodes());
        auto stored_already = cluster_local_map.find(n_data_curr_comm_id);
        if (stored_already != cluster_local_map.end()) {
          n_data_curr_comm_id = stored_already->second;
        } else {
          cluster_local_map[n_data_curr_comm_id] = num_unique_clusters;
          n_data_curr_comm_id = num_unique_clusters;
          num_unique_clusters++;
        }
      }
    }
    return num_unique_clusters;
  }

  template <typename EdgeWeightType>
  static void CheckModularity(
      Graph& graph, katana::NUMAArray<uint64_t>& clusters_orig) {
    katana::do_all(katana::iterate(graph), [&](GNode n) {
      graph.template GetData<CurrentCommunityID>(n).curr_comm_ass =
          clusters_orig[n];
    });

    [[maybe_unused]] uint64_t num_unique_clusters =
        RenumberClustersContiguously(graph);
    auto mod = CalModularityFinal<EdgeWeightType, CurrentCommunityID>(graph);
  }

  /**
 * Creates a duplicate of the graph by copying the
 * graph (pfg_from) topology
 * (read from the underlying RDG) to the in-memory
 * temporary graph (pfg_to).
 * TODO(gill) replace with ephemeral graph
 */
  static katana::Result<std::unique_ptr<katana::PropertyGraph>>
  DuplicateGraphWithSameTopo(const katana::PropertyGraph& pfg_from) {
    const katana::GraphTopology& topology_from = pfg_from.topology();

    katana::GraphTopology topo_copy = GraphTopology::Copy(topology_from);

    auto pfg_to_res = katana::PropertyGraph::Make(std::move(topo_copy));
    if (!pfg_to_res) {
      return pfg_to_res.error();
    }
    return std::unique_ptr<katana::PropertyGraph>(
        std::move(pfg_to_res.value()));
  }

  /**
 * Copy edge property from
 * property graph, pg_from to pg_to.
 */
  static katana::Result<void> CopyEdgeProperty(
      katana::PropertyGraph* pfg_from, katana::PropertyGraph* pfg_to,
      const std::string& edge_property_name,
      const std::string& new_edge_property_name) {
    // Remove the existing edge property
    if (pfg_to->HasEdgeProperty(new_edge_property_name)) {
      if (auto r = pfg_to->RemoveEdgeProperty(new_edge_property_name); !r) {
        return r.error();
      }
    }
    // Copy edge properties
    using ArrowType = typename arrow::CTypeTraits<EdgeTy>::ArrowType;
    auto edge_property_result =
        pfg_from->GetEdgePropertyTyped<EdgeTy>(edge_property_name);
    if (!edge_property_result) {
      return edge_property_result.error();
    }
    auto edge_property = edge_property_result.value();
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> columns;
    fields.emplace_back(
        arrow::field((new_edge_property_name), std::make_shared<ArrowType>()));
    columns.emplace_back(edge_property);
    auto edge_data_table = arrow::Table::Make(arrow::schema(fields), columns);
    if (auto r = pfg_to->AddEdgeProperties(edge_data_table); !r) {
      return r.error();
    }
    return katana::ResultSuccess();
  }

  /**
 * Creates a coarsened hierarchical graph for the next phase
 * of the clustering algorithm. It merges all the nodes within a
 * same cluster to form a super node for the coursened graphs.
 * The total number of nodes in the coarsened graph are equal to
 * the number of unique clusters in the previous level of the graph.
 * All the edges inside a cluster are merged (edge weights are summed
 * up) to form the edges within super nodes.
 */
  template <
      typename NodeData, typename EdgeData, typename EdgeWeightType,
      typename CommunityIDType>
  static katana::Result<std::unique_ptr<katana::PropertyGraph>> GraphCoarsening(
      const Graph& graph, katana::PropertyGraph* pfg_mutable,
      uint64_t num_unique_clusters,
      const std::vector<std::string>& temp_node_property_names,
      const std::vector<std::string>& temp_edge_property_names) {
    using GNode = typename Graph::Node;

    katana::StatTimer TimerGraphBuild("Timer_Graph_build");
    TimerGraphBuild.start();

    const uint64_t num_nodes_next = num_unique_clusters;

    std::vector<std::vector<GNode>> cluster_bags(num_unique_clusters);
    // TODO(amber): This loop can be parallelized when using a concurrent container
    // for cluster_bags, but something like katana::InsertBag exhausts the
    // per-thread-storage memory
    for (GNode n = 0; n < graph.num_nodes(); ++n) {
      auto n_data_curr_comm_id = graph.template GetData<CommunityIDType>(n);
      if (n_data_curr_comm_id != UNASSIGNED) {
        cluster_bags[n_data_curr_comm_id].push_back(n);
      }
    }

    std::vector<katana::gstl::Vector<uint32_t>> edges_id(num_unique_clusters);
    std::vector<katana::gstl::Vector<EdgeTy>> edges_data(num_unique_clusters);

    /* First pass to find the number of edges */
    katana::do_all(
        katana::iterate(uint64_t{0}, num_unique_clusters),
        [&](uint64_t c) {
          katana::gstl::Map<uint64_t, uint64_t> cluster_local_map;
          uint64_t num_unique_clusters = 0;
          for (auto node : cluster_bags[c]) {
            KATANA_LOG_DEBUG_ASSERT(
                graph.template GetData<CommunityIDType>(node) ==
                c);  // All nodes in this bag must have same cluster id

            for (auto ii = graph.edge_begin(node); ii != graph.edge_end(node);
                 ++ii) {
              auto dst = graph.GetEdgeDest(ii);
              auto dst_data_curr_comm_id =
                  graph.template GetData<CommunityIDType>(dst);
              KATANA_LOG_DEBUG_ASSERT(dst_data_curr_comm_id != UNASSIGNED);
              auto stored_already = cluster_local_map.find(
                  dst_data_curr_comm_id);  // Check if it already exists
              if (stored_already != cluster_local_map.end()) {
                edges_data[c][stored_already->second] +=
                    graph.template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
              } else {
                cluster_local_map[dst_data_curr_comm_id] = num_unique_clusters;
                edges_id[c].push_back(dst_data_curr_comm_id);
                edges_data[c].push_back(
                    graph.template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii));
                num_unique_clusters++;
              }
            }  // End edge loop
          }
        },
        katana::steal(), katana::loopname("BuildGraph: Find edges"));

    /* Serial loop to reduce all the edge counts */
    katana::NUMAArray<uint64_t> prefix_edges_count;
    prefix_edges_count.allocateInterleaved(num_unique_clusters);

    katana::GAccumulator<uint64_t> num_edges_acc;
    katana::do_all(
        katana::iterate(uint64_t{0}, num_nodes_next), [&](uint64_t c) {
          prefix_edges_count[c] = edges_id[c].size();
          num_edges_acc += prefix_edges_count[c];
        });

    const uint64_t num_edges_next = num_edges_acc.reduce();

    katana::ParallelSTL::partial_sum(
        prefix_edges_count.begin(), prefix_edges_count.end(),
        prefix_edges_count.begin());

    KATANA_LOG_DEBUG_ASSERT(
        prefix_edges_count[num_unique_clusters - 1] == num_edges_next);
    katana::StatTimer TimerConstructFrom("Timer_Construct_From");
    TimerConstructFrom.start();

    // Remove all the existing node/edge properties
    for (auto property : temp_node_property_names) {
      if (pfg_mutable->HasNodeProperty(property)) {
        if (auto r = pfg_mutable->RemoveNodeProperty(property); !r) {
          return r.error();
        }
      }
    }
    for (auto property : temp_edge_property_names) {
      if (pfg_mutable->HasEdgeProperty(property)) {
        if (auto r = pfg_mutable->RemoveEdgeProperty(property); !r) {
          return r.error();
        }
      }
    }

    using Node = katana::GraphTopology::Node;
    using Edge = katana::GraphTopology::Edge;

    katana::NUMAArray<Node> out_dests_next;
    out_dests_next.allocateInterleaved(num_edges_next);

    katana::NUMAArray<EdgeWeightType> edge_data_next;
    edge_data_next.allocateInterleaved(num_edges_next);

    katana::do_all(
        katana::iterate(uint64_t{0}, num_nodes_next), [&](uint64_t n) {
          uint64_t number_of_edges =
              (n == 0) ? prefix_edges_count[0]
                       : (prefix_edges_count[n] - prefix_edges_count[n - 1]);
          uint64_t start_index = (n == 0) ? 0 : prefix_edges_count[n - 1];
          for (uint64_t k = 0; k < number_of_edges; ++k) {
            out_dests_next[start_index + k] = edges_id[n][k];
            edge_data_next[start_index + k] = edges_data[n][k];
          }
        });

    TimerConstructFrom.stop();

    // TODO(amber): This is a lame attempt at freeing the memory back to each
    // thread's pool of free pages and blocks. Due to stealing, the execution of
    // do_all above that populates these containers may be different from the
    // do_all below that frees them.
    katana::do_all(
        katana::iterate(uint64_t{0}, num_unique_clusters), [&](uint64_t c) {
          edges_id[c] = gstl::Vector<uint32_t>();
          edges_data[c] = gstl::Vector<EdgeTy>();
        });

    GraphTopology topo_next{
        std::move(prefix_edges_count), std::move(out_dests_next)};
    auto pfg_next_res = katana::PropertyGraph::Make(std::move(topo_next));

    if (!pfg_next_res) {
      return pfg_next_res.error();
    }
    std::unique_ptr<katana::PropertyGraph> pfg_next =
        std::move(pfg_next_res.value());

    if (auto result = katana::analytics::ConstructNodeProperties<NodeData>(
            pfg_next.get(), temp_node_property_names);
        !result) {
      return result.error();
    }

    if (auto result = katana::analytics::ConstructEdgeProperties<EdgeData>(
            pfg_next.get(), temp_edge_property_names);
        !result) {
      return result.error();
    }

    auto graph_result = Graph::Make(pfg_next.get());
    if (!graph_result) {
      return graph_result.error();
    }
    Graph graph_curr = graph_result.value();
    // TODO(amber): figure out a better way to add/update the edge property
    katana::do_all(
        katana::iterate(Edge{0}, num_edges_next),
        [&](Edge e) {
          graph_curr.template GetEdgeData<EdgeWeight<EdgeWeightType>>(e) =
              edge_data_next[e];
        },
        katana::no_stats());

    TimerGraphBuild.stop();
    return std::unique_ptr<katana::PropertyGraph>(std::move(pfg_next));
  }

  /**
 * Functions specific to Leiden clustering
 */

  /**
   * Sums up the degree weight for all
   * the unique clusters.
   */
  template <typename EdgeWeightType>
  static void SumVertexDegreeWeightWithNodeWeight(Graph* graph) {
    katana::do_all(katana::iterate(*graph), [&](GNode n) {
      EdgeTy total_weight = 0;
      auto& n_degree_wt =
          graph->template GetData<DegreeWeight<EdgeWeightType>>(n);

      for (auto ii = graph->edge_begin(n); ii != graph->edge_end(n); ++ii) {
        total_weight +=
            graph->template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
      }
      n_degree_wt = total_weight;
    });
    return;
  }

  template <typename ValTy>
  static uint64_t GenerateRandonNumber(ValTy min, ValTy max) {
    std::random_device
        rd;  // Will be used to obtain a seed for the random number engine
    std::mt19937 gen(
        rd());  // Standard mersenne_twister_engine seeded with rd()
    std::uniform_real_distribution<> dis(
        min,
        max);  // distribution in range [min, max]
    return dis(gen);
  }

  template <typename EdgeWeightType>
  static uint64_t GetRandomSubcommunity(
      const Graph& graph, GNode n, CommunityArray& subcomm_info,
      uint64_t total_degree_wt, uint64_t comm_id,
      double constant_for_second_term, double resolution, double randomness) {
    auto& n_current_subcomm_id =
        graph.template GetData<CurrentSubCommunityID>(n);
    /*
   * Remove the currently selected node from its current cluster.
   * This causes the cluster to be empty.
   */
    subcomm_info[n_current_subcomm_id].node_wt = 0;
    subcomm_info[n_current_subcomm_id].internal_edge_wt = 0;

    /*
   * Map each neighbor's subcommunity to local number: Subcommunity --> Index
   */
    std::map<uint64_t, uint64_t> cluster_local_map;

    /*
   * Edges weight to each unique subcommunity
   */
    std::vector<EdgeTy> counter;
    std::vector<uint64_t> neighboring_cluster_ids;

    /*
   * Identify the neighboring clusters of the currently selected
   * node, that is, the clusters with which the currently
   * selected node is connected. The old cluster of the currently
   * selected node is also included in the set of neighboring
   * clusters. In this way, it is always possible that the
   * currently selected node will be moved back to its old
   * cluster.
   */
    cluster_local_map[n_current_subcomm_id] = 0;  // Add n's current
                                                  // subcommunity
    counter.push_back(0);  // Initialize the counter to zero (no edges incident
                           // yet)
    neighboring_cluster_ids.push_back(n_current_subcomm_id);
    uint64_t num_unique_clusters = 1;

    EdgeTy self_loop_wt = 0;

    for (auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
      auto dst = graph.GetEdgeDest(ii);
      EdgeWeightType edge_wt =
          graph.template GetEdgeData<EdgeWeight<EdgeWeightType>>(
              ii);  // Self loop weights is recorded
      auto& n_current_comm = graph.template GetData<CurrentCommunityID>(dst);
      auto& n_current_subcomm =
          graph.template GetData<CurrentSubCommunityID>(dst);

      if (n_current_comm == comm_id) {
        if (*dst == n) {
          self_loop_wt += edge_wt;  // Self loop weights is recorded
        }
        auto stored_already = cluster_local_map.find(
            n_current_subcomm);  // Check if it already exists
        if (stored_already != cluster_local_map.end()) {
          counter[stored_already->second] += edge_wt;
        } else {
          cluster_local_map[n_current_subcomm] = num_unique_clusters;
          counter.push_back(edge_wt);
          neighboring_cluster_ids.push_back(n_current_subcomm);
          num_unique_clusters++;
        }
      }
    }  // End edge loop

    uint64_t best_cluster = n_current_subcomm_id;
    double max_quality_value_increment = 0;
    double total_transformed_quality_value_increment = 0;
    double quality_value_increment = 0;
    std::vector<double> cum_transformed_quality_value_increment_per_cluster(
        num_unique_clusters);
    auto& n_node_wt = graph.template GetData<NodeWeight>(n);
    for (auto pair : cluster_local_map) {
      auto subcomm = pair.first;
      if (n_current_subcomm_id == subcomm)
        continue;

      uint64_t subcomm_node_wt = subcomm_info[subcomm].node_wt;
      uint64_t subcomm_degree_wt = subcomm_info[subcomm].degree_wt;

      // check if subcommunity is well connected
      if (subcomm_info[subcomm].internal_edge_wt >=
          constant_for_second_term * (double)subcomm_degree_wt *
              ((double)total_degree_wt - (double)subcomm_degree_wt)) {
        quality_value_increment =
            counter[pair.second] - n_node_wt * subcomm_node_wt * resolution;

        if (quality_value_increment > max_quality_value_increment) {
          best_cluster = subcomm;
          max_quality_value_increment = quality_value_increment;
        }

        if (quality_value_increment >= 0)
          total_transformed_quality_value_increment +=
              std::exp(quality_value_increment / randomness);
      }
      cum_transformed_quality_value_increment_per_cluster[pair.second] =
          total_transformed_quality_value_increment;
      counter[pair.second] = 0;
    }

    /*
   * Determine the neighboring cluster to which the currently
   * selected node will be moved.
   */
    int64_t min_idx, max_idx, mid_idx;
    uint64_t chosen_cluster;
    double r;
    if (total_transformed_quality_value_increment < INFINITY_DOUBLE) {
      r = total_transformed_quality_value_increment *
          GenerateRandonNumber(0.0, 1.0);
      min_idx = -1;
      max_idx = num_unique_clusters + 1;
      while (min_idx < max_idx - 1) {
        mid_idx = (min_idx + max_idx) / 2;
        if (cum_transformed_quality_value_increment_per_cluster[mid_idx] >= r)
          max_idx = mid_idx;
        else
          min_idx = mid_idx;
      }
      chosen_cluster = neighboring_cluster_ids[max_idx];
    } else {
      chosen_cluster = best_cluster;
    }
    return chosen_cluster;
  }

  /**
 * Finds a clustering of the nodes in a network using the local merging
 * algorithm.
 *
 * <p>
 * The local merging algorithm starts from a singleton partition. It
 * performs a single iteration over the nodes in a network. Each node
 * belonging to a singleton cluster is considered for merging with another
 * cluster. This cluster is chosen randomly from all clusters that do not
 * result in a decrease in the quality function. The larger the increase in
 * the quality function, the more likely a cluster is to be chosen. The
 * strength of this effect is determined by the randomness parameter. The
 * higher the value of the randomness parameter, the stronger the
 * randomness in the choice of a cluster. The lower the value of the
 * randomness parameter, the more likely the cluster resulting in the
 * largest increase in the quality function is to be chosen. A node is
 * merged with a cluster only if both are sufficiently well connected to
 * the rest of the network.
 * </p>
 *
 * @param
 *
 * @return : Number of unique subcommunities formed
 * DO NOT parallelize as it is called within Galois parallel loops
 *
 */
  template <typename EdgeWeightType>
  static void MergeNodesSubset(
      Graph* graph, std::vector<GNode>& cluster_nodes, uint64_t comm_id,
      uint64_t total_degree_wt, CommunityArray& subcomm_info,
      double constant_for_second_term, double resolution, double randomness) {
    // select set R
    std::vector<GNode> cluster_nodes_to_move;
    for (uint64_t i = 0; i < cluster_nodes.size(); ++i) {
      GNode n = cluster_nodes[i];
      auto& n_degree_wt =
          graph->template GetData<DegreeWeight<EdgeWeightType>>(n);
      auto& n_node_wt = graph->template GetData<NodeWeight>(n);
      /*
     * Initialize with singleton sub-communities
     */
      EdgeWeightType node_edge_weight_within_cluster = 0;
      for (auto ii = graph->edge_begin(n); ii != graph->edge_end(n); ++ii) {
        auto dst = graph->GetEdgeDest(ii);
        EdgeWeightType edge_wt =
            graph->template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
        /*
       * Must include the edge weight of all neighbors excluding self loops
       * belonging to the community comm_id
       */
        if (*dst != n &&
            graph->template GetData<CurrentCommunityID>(dst) == comm_id) {
          node_edge_weight_within_cluster += edge_wt;
        }
      }

      uint64_t node_wt = n_node_wt;
      uint64_t degree_wt = n_degree_wt;
      /*
     * Additionally, only nodes that are well connected with
     * the rest of the network are considered for moving.
     * (externalEdgeWeightPerCluster[j] >= clusterWeights[j] * (totalNodeWeight
     * - clusterWeights[j]) * resolution
     */
      if (node_edge_weight_within_cluster >=
          constant_for_second_term * (double)degree_wt *
              ((double)total_degree_wt - (double)degree_wt))
        cluster_nodes_to_move.push_back(n);

      subcomm_info[n].node_wt = node_wt;
      subcomm_info[n].internal_edge_wt = node_edge_weight_within_cluster;
      subcomm_info[n].size = 1;
      subcomm_info[n].degree_wt = degree_wt;
    }

    for (GNode n : cluster_nodes_to_move) {
      auto& n_degree_wt =
          graph->template GetData<DegreeWeight<EdgeWeightType>>(n);
      auto& n_node_wt = graph->template GetData<NodeWeight>(n);
      auto& n_current_subcomm_id =
          graph->template GetData<CurrentSubCommunityID>(n);
      /*
     * Only consider singleton communities
     */
      if (subcomm_info[n_current_subcomm_id].size == 1) {
        uint64_t new_subcomm_ass = GetRandomSubcommunity<EdgeWeightType>(
            *graph, n, subcomm_info, total_degree_wt, comm_id,
            constant_for_second_term, resolution, randomness);

        if ((int64_t)new_subcomm_ass != -1 &&
            new_subcomm_ass !=
                graph->template GetData<CurrentSubCommunityID>(n)) {
          n_current_subcomm_id = new_subcomm_ass;

          /*
         * Move the currently selected node to its new cluster and
         * update the clustering statistics.
         */
          katana::atomicAdd(subcomm_info[new_subcomm_ass].node_wt, n_node_wt);
          katana::atomicAdd(subcomm_info[new_subcomm_ass].size, (uint64_t)1);
          katana::atomicAdd(
              subcomm_info[new_subcomm_ass].degree_wt, n_degree_wt);

          for (auto ii = graph->edge_begin(n); ii != graph->edge_end(n); ++ii) {
            auto dst = graph->GetEdgeDest(ii);
            auto edge_wt =
                graph->template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
            if (*dst != n &&
                graph->template GetData<CurrentCommunityID>(dst) == comm_id) {
              if (graph->template GetData<CurrentSubCommunityID>(dst) ==
                  new_subcomm_ass) {
                subcomm_info[new_subcomm_ass].internal_edge_wt -= edge_wt;
              } else {
                subcomm_info[new_subcomm_ass].internal_edge_wt += edge_wt;
              }
            }
          }
        }
      }
    }
  }

  /*
 * Refine the clustering by iterating over the clusters and by
 * trying to split up each cluster into multiple clusters.
 */
  template <typename EdgeWeightType>
  static void RefinePartition(
      Graph* graph, double resolution, double randomness) {
    double constant_for_second_term =
        CalConstantForSecondTerm<EdgeWeightType>(*graph);
    // set singleton subcommunities
    katana::do_all(
        katana::iterate(*graph),
        [&](GNode n) { graph->template GetData<CurrentSubCommunityID>(n) = n; },
        katana::steal());

    // populate nodes into communities
    std::vector<std::vector<GNode>> cluster_bags(2 * graph->size() + 1);
    CommunityArray comm_info;

    comm_info.allocateBlocked(2 * graph->size() + 1);

    katana::do_all(
        katana::iterate((uint32_t)0, (uint32_t)(2 * graph->size() + 1)),
        [&](uint32_t n) {
          comm_info[n].node_wt = (uint64_t)0;
          comm_info[n].degree_wt = (uint64_t)0;
        },
        katana::steal());

    for (GNode n : *graph) {
      auto& n_current_comm = graph->template GetData<CurrentCommunityID>(n);
      auto& n_node_wt = graph->template GetData<NodeWeight>(n);
      auto& n_degree_wt =
          graph->template GetData<DegreeWeight<EdgeWeightType>>(n);
      if (n_current_comm != UNASSIGNED) {
        cluster_bags[n_current_comm].push_back(n);

        katana::atomicAdd(comm_info[n_current_comm].node_wt, n_node_wt);
        katana::atomicAdd(comm_info[n_current_comm].degree_wt, n_degree_wt);
      }
    }

    CommunityArray subcomm_info;

    subcomm_info.allocateBlocked(graph->size() + 1);

    // call MergeNodesSubset for each community in parallel
    katana::do_all(
        katana::iterate((uint64_t)0, (uint64_t)graph->size()), [&](uint64_t c) {
          /*
                    * Only nodes belonging to singleton clusters can be moved to
                    * a different cluster. This guarantees that clusters will
                    * never be split up.
                    */
          comm_info[c].num_sub_communities = 0;
          if (cluster_bags[c].size() > 1) {
            MergeNodesSubset<EdgeWeightType>(
                graph, cluster_bags[c], c, comm_info[c].degree_wt, subcomm_info,
                constant_for_second_term, resolution, randomness);
          } else {
            comm_info[c].num_sub_communities = 0;
          }
        });
  }

  template <typename EdgeWeightType>
  uint64_t MaxCPMQualityWithoutSwaps(
      std::map<uint64_t, uint64_t>& cluster_local_map,
      std::vector<EdgeWeightType>& counter, EdgeWeightType self_loop_wt,
      CommunityArray& c_info, uint64_t node_wt, uint64_t sc,
      double resolution) {
    uint64_t max_index = sc;  // Assign the initial value as self community
    double cur_gain = 0;
    double max_gain = 0;
    double eix = counter[0] - self_loop_wt;
    double eiy = 0;
    double size_x = (double)(c_info[sc].node_wt - node_wt);
    double size_y = 0;

    auto stored_already = cluster_local_map.begin();
    do {
      if (sc != stored_already->first) {
        eiy = counter[stored_already
                          ->second];  // Total edges incident on cluster y
        size_y = c_info[stored_already->first].node_wt;

        cur_gain = 2.0f * (double)(eiy - eix) -
                   resolution * node_wt * (double)(size_y - size_x);
        if ((cur_gain > max_gain) ||
            ((cur_gain == max_gain) && (cur_gain != 0) &&
             (stored_already->first < max_index))) {
          max_gain = cur_gain;
          max_index = stored_already->first;
        }
      }
      stored_already++;  // Explore next cluster
    } while (stored_already != cluster_local_map.end());

    if ((c_info[max_index].size == 1 && c_info[sc].size == 1 &&
         max_index > sc)) {
      max_index = sc;
    }
    assert(max_gain >= 0);
    return max_index;
  }

  template <typename EdgeWeightType>
  double CalCPMQuality(
      Graph& graph, CommunityArray& c_info, double& e_xx, double& a2_x,
      double& constant_for_second_term, double resolution) {
    /* Variables needed for Modularity calculation */
    double mod = -1;

    katana::NUMAArray<EdgeWeightType> cluster_wt_internal;

    /*** Initialization ***/
    cluster_wt_internal.allocateBlocked(graph.size());

    /* Calculate the overall modularity */
    katana::GAccumulator<double> acc_e_xx;
    katana::GAccumulator<double> acc_a2_x;

    katana::do_all(
        katana::iterate(graph), [&](GNode n) { cluster_wt_internal[n] = 0; });

    katana::do_all(katana::iterate(graph), [&](GNode n) {
      auto& n_data_curr_comm = graph.template GetData<CurrentSubCommunityID>(n);
      for (auto ii = graph.edge_begin(n); ii != graph.edge_end(n); ++ii) {
        if (graph.template GetData<CurrentSubCommunityID>(
                graph.GetEdgeDest(ii)) == n_data_curr_comm) {
          cluster_wt_internal[n] +=
              graph.template GetEdgeData<EdgeWeight<EdgeWeightType>>(ii);
        }
      }
    });

    katana::do_all(katana::iterate(graph), [&](GNode n) {
      acc_e_xx += cluster_wt_internal[n];
      // acc_a2_x +=
      //     (double)(c_info[n].node_wt) * ((double)(c_info[n].node_wt - 1) * 0.5f);
      acc_a2_x += (double)(c_info[n].node_wt) *
                  ((double)(c_info[n].node_wt) * resolution);
    });

    e_xx = acc_e_xx.reduce();
    a2_x = acc_a2_x.reduce();
    mod = (e_xx - a2_x) * (double)constant_for_second_term;

    return mod;
  }
};
}  // namespace katana::analytics
#endif  // CLUSTERING_H
