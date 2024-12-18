#pragma once

#include "mls/common.h"
#include "mls/core_types.h"
#include "mls/crypto.h"
#include "mls/tree_math.h"
#include <namespace.h>
#include <tls/tls_syntax.h>

#define ENABLE_TREE_DUMP 1

namespace MLS_NAMESPACE {

enum struct NodeType : uint8_t
{
  reserved = 0x00,
  leaf = 0x01,
  parent = 0x02,
};

struct Node
{
  var::variant<LeafNode, ParentNode> node;

  const HPKEPublicKey& public_key() const;
  std::optional<bytes> parent_hash() const;

  TLS_SERIALIZABLE(node)
  TLS_TRAITS(tls::variant<NodeType>)
};

struct OptionalNode
{
  std::optional<Node> node;

  bool blank() const { return !node.has_value(); }
  bool leaf() const
  {
    return !blank() && var::holds_alternative<LeafNode>(opt::get(node).node);
  }

  LeafNode& leaf_node() { return var::get<LeafNode>(opt::get(node).node); }

  const LeafNode& leaf_node() const
  {
    return var::get<LeafNode>(opt::get(node).node);
  }

  ParentNode& parent_node()
  {
    return var::get<ParentNode>(opt::get(node).node);
  }

  const ParentNode& parent_node() const
  {
    return var::get<ParentNode>(opt::get(node).node);
  }

  TLS_SERIALIZABLE(node)
};

struct TreeSlice
{
  LeafIndex leaf_index;
  LeafCount n_leaves;
  std::vector<OptionalNode> direct_path_nodes;
  std::vector<bytes> copath_hashes;

  bytes tree_hash(CipherSuite suite) const;

  TLS_SERIALIZABLE(leaf_index, n_leaves, direct_path_nodes, copath_hashes);
};

struct TreeKEMPublicKey;

struct TreeKEMPrivateKey
{
  CipherSuite suite;
  LeafIndex index;
  bytes update_secret;
  std::map<NodeIndex, bytes> path_secrets;
  std::map<NodeIndex, HPKEPrivateKey> private_key_cache;

  static TreeKEMPrivateKey solo(CipherSuite suite,
                                LeafIndex index,
                                HPKEPrivateKey leaf_priv);
  static TreeKEMPrivateKey create(const TreeKEMPublicKey& pub,
                                  LeafIndex from,
                                  const bytes& leaf_secret);
  static TreeKEMPrivateKey joiner(const TreeKEMPublicKey& pub,
                                  LeafIndex index,
                                  HPKEPrivateKey leaf_priv,
                                  NodeIndex intersect,
                                  const std::optional<bytes>& path_secret);

  void set_leaf_priv(HPKEPrivateKey priv);
  std::tuple<NodeIndex, bytes, bool> shared_path_secret(LeafIndex to) const;

  bool have_private_key(NodeIndex n) const;
  std::optional<HPKEPrivateKey> private_key(NodeIndex n);
  std::optional<HPKEPrivateKey> private_key(NodeIndex n) const;

  void decap(LeafIndex from,
             const TreeKEMPublicKey& pub,
             const bytes& context,
             const UpdatePath& path,
             const std::vector<LeafIndex>& except);

  void decap(LeafIndex from,
             const TreeKEMPublicKey& pub,
             const bytes& context,
             const NodeIndex& decrypt_node,
             const HPKECiphertext& encrypted_path_secret);

  void truncate(LeafCount size);

  bool consistent(const TreeKEMPrivateKey& other) const;
  bool consistent(const TreeKEMPublicKey& other) const;

#if ENABLE_TREE_DUMP
  void dump() const;
#endif

  // TODO(RLB) Make this private but exposed to test vectors
  void implant(const TreeKEMPublicKey& pub,
               NodeIndex start,
               const bytes& path_secret);
  void implant_matching(const TreeKEMPublicKey& pub,
                        NodeIndex start,
                        const bytes& path_secret);
};

struct TreeKEMPublicKey
{
  CipherSuite suite;
  LeafCount size{ 0 };
  std::map<NodeIndex, OptionalNode> nodes;

  explicit TreeKEMPublicKey(CipherSuite suite);
  TreeKEMPublicKey(CipherSuite suite, const TreeSlice& slice);

  TreeKEMPublicKey() = default;
  TreeKEMPublicKey(const TreeKEMPublicKey& other) = default;
  TreeKEMPublicKey(TreeKEMPublicKey&& other) = default;
  TreeKEMPublicKey& operator=(const TreeKEMPublicKey& other) = default;
  TreeKEMPublicKey& operator=(TreeKEMPublicKey&& other) = default;

  LeafIndex allocate_leaf();
  LeafIndex add_leaf(const LeafNode& leaf);
  void update_leaf(LeafIndex index, const LeafNode& leaf);
  void blank_path(LeafIndex index);

  TreeKEMPrivateKey update(LeafIndex from,
                           const bytes& leaf_secret,
                           const bytes& group_id,
                           const SignaturePrivateKey& sig_priv,
                           const LeafNodeOptions& opts);
  UpdatePath encap(const TreeKEMPrivateKey& priv,
                   const bytes& context,
                   const std::vector<LeafIndex>& except) const;

  void merge(LeafIndex from, const UpdatePath& path);
  void set_hash_all();
  const bytes& get_hash(NodeIndex index);
  bytes root_hash() const;

  bool parent_hash_valid(LeafIndex from) const;
  bool parent_hash_valid(LeafIndex from, const UpdatePath& path) const;
  bool parent_hash_valid() const;
  bool is_complete() const;

  bool has_leaf(LeafIndex index) const;
  std::optional<LeafIndex> find(const LeafNode& leaf) const;
  std::optional<LeafNode> leaf_node(LeafIndex index) const;
  std::vector<NodeIndex> resolve(NodeIndex index) const;

  TreeSlice extract_slice(LeafIndex leaf) const;
  void implant_slice(const TreeSlice& slice);
  std::tuple<HPKECiphertext, NodeIndex> slice_path(UpdatePath path,
                                                   LeafIndex from,
                                                   LeafIndex to) const;

  struct AncestorIndex
  {
    size_t ancestor_node_index;
    NodeIndex resolution_node;
  };
  AncestorIndex ancestor_index(LeafIndex to, LeafIndex from) const;

  struct DecapCoords
  {
    size_t ancestor_node_index;
    size_t resolution_node_index;
    NodeIndex resolution_node;
  };
  DecapCoords decap_coords(
    LeafIndex to,
    LeafIndex from,
    const std::vector<LeafIndex>& joiner_locations) const;

  template<typename UnaryPredicate>
  bool all_leaves(const UnaryPredicate& pred) const
  {
    for (LeafIndex i{ 0 }; i < size; i.val++) {
      // Only test known nodes
      // XXX(RLB) This could be dangerous, since it allows for nodes to fail the
      // predicate as long as they are unknown.
      if (nodes.count(NodeIndex(i)) == 0) {
        continue;
      }

      const auto& node = node_at(i);
      if (node.blank()) {
        continue;
      }

      if (!pred(i, node.leaf_node())) {
        return false;
      }
    }

    return true;
  }

  template<typename UnaryPredicate>
  bool any_leaf(const UnaryPredicate& pred) const
  {
    for (LeafIndex i{ 0 }; i < size; i.val++) {
      const auto& node = node_at(i);
      if (node.blank()) {
        continue;
      }

      if (pred(i, node.leaf_node())) {
        return true;
      }
    }

    return false;
  }

  using FilteredDirectPathEntry = std::tuple<NodeIndex, std::vector<NodeIndex>>;
  using FilteredDirectPath = std::vector<FilteredDirectPathEntry>;
  FilteredDirectPath filtered_direct_path(NodeIndex index) const;

  void truncate();

  OptionalNode& node_at(NodeIndex n);
  const OptionalNode& node_at(NodeIndex n) const;
  OptionalNode& node_at(LeafIndex n);
  const OptionalNode& node_at(LeafIndex n) const;

  TLS_SERIALIZABLE(nodes)

#if ENABLE_TREE_DUMP
  void dump() const;
#endif

private:
  std::map<NodeIndex, bytes> hashes;

  void clear_hash_all();
  void clear_hash_path(LeafIndex index);

  bool has_parent_hash(NodeIndex child, const bytes& target_ph) const;
  bool parent_hash_valid(LeafIndex from,
                         const UpdatePath& path,
                         const FilteredDirectPath& fdp) const;

  bytes parent_hash(const ParentNode& parent, NodeIndex copath_child) const;
  std::vector<bytes> parent_hashes(
    LeafIndex from,
    const FilteredDirectPath& fdp,
    const std::vector<UpdatePathNode>& path_nodes) const;

  using TreeHashCache = std::map<NodeIndex, std::pair<size_t, bytes>>;
  const bytes& original_tree_hash(TreeHashCache& cache,
                                  NodeIndex index,
                                  std::vector<LeafIndex> parent_except) const;
  bytes original_parent_hash(TreeHashCache& cache,
                             NodeIndex parent,
                             NodeIndex sibling) const;

  bool exists_in_tree(const HPKEPublicKey& key,
                      std::optional<LeafIndex> except) const;
  bool exists_in_tree(const SignaturePublicKey& key,
                      std::optional<LeafIndex> except) const;

  void implant_slice_unchecked(const TreeSlice& slice);

  OptionalNode blank_node;

  friend struct TreeKEMPrivateKey;
};

tls::ostream&
operator<<(tls::ostream& str, const TreeKEMPublicKey& obj);
tls::istream&
operator>>(tls::istream& str, TreeKEMPublicKey& obj);

struct LeafNodeHashInput;
struct ParentNodeHashInput;

} // namespace MLS_NAMESPACE

namespace MLS_NAMESPACE::tls {

TLS_VARIANT_MAP(MLS_NAMESPACE::NodeType, MLS_NAMESPACE::LeafNodeHashInput, leaf)
TLS_VARIANT_MAP(MLS_NAMESPACE::NodeType,
                MLS_NAMESPACE::ParentNodeHashInput,
                parent)

TLS_VARIANT_MAP(MLS_NAMESPACE::NodeType, MLS_NAMESPACE::LeafNode, leaf)
TLS_VARIANT_MAP(MLS_NAMESPACE::NodeType, MLS_NAMESPACE::ParentNode, parent)

} // namespace MLS_NAMESPACE::tls
