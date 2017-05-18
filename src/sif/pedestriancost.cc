#include "sif/pedestriancost.h"
#include "sif/costconstants.h"

#include "baldr/accessrestriction.h"
#include "midgard/constants.h"
#include "midgard/logging.h"

#ifdef INLINE_TEST
#include "test/test.h"
#include <random>
#include <boost/property_tree/json_parser.hpp>
#endif

using namespace valhalla::baldr;

namespace valhalla {
namespace sif {

// Default options/values
namespace {

// Maximum route distances
constexpr uint32_t kMaxDistanceFoot        = 100000; // 100 km
constexpr uint32_t kMaxDistanceWheelchair  = 10000;  // 10 km

// Default speeds
constexpr float kDefaultSpeedFoot          = 5.1f;   // 3.16 MPH
constexpr float kDefaultSpeedWheelchair    = 4.0f;   // 2.5  MPH  TODO

// Penalty to take steps
constexpr float kDefaultStepPenaltyFoot       = 30.0f;   // 30 seconds
constexpr float kDefaultStepPenaltyWheelchair = 600.0f;  // 10 minutes

// Maximum grade30
constexpr uint32_t kDefaultMaxGradeFoot = 90;
constexpr uint32_t kDefaultMaxGradeWheelchair = 12; // Conservative for now...

// Other defaults (not dependent on type)
constexpr float kModeWeight             = 1.5f;   // Favor this mode?
constexpr float kDefaultManeuverPenalty = 5.0f;   // Seconds
constexpr float kDefaultGatePenalty     = 10.0f;  // Seconds
constexpr float kDefaultWalkwayFactor   = 0.9f;   // Slightly favor walkways
constexpr float kDefaultSideWalkFactor  = 0.95f;  // Slightly favor sidewalks
constexpr float kDefaultAlleyFactor     = 2.0f;   // Avoid alleys
constexpr float kDefaultDrivewayFactor  = 5.0f;   // Avoid driveways
constexpr float kDefaultFerryCost               = 300.0f; // Seconds
constexpr float kDefaultCountryCrossingCost     = 600.0f; // Seconds
constexpr float kDefaultCountryCrossingPenalty  = 0.0f;   // Seconds
constexpr float kDefaultUseFerry = 1.0f;

// Maximum distance at the beginning or end of a multimodal route
// that you are willing to travel for this mode.  In this case,
// it is the max walking distance.
constexpr uint32_t kTransitStartEndMaxDistance    = 2415;   // 1.5 miles

// Maximum transfer distance between stops that you are willing
// to travel for this mode.  In this case, it is the max walking
// distance you are willing to walk between transfers.
constexpr uint32_t kTransitTransferMaxDistance   = 805;   // 0.5 miles

// Avoid roundabouts
constexpr float kRoundaboutFactor = 5.0f;

// Maximum ferry penalty (when use_ferry == 0). Can't make this too large
// since a ferry is sometimes required to complete a route.
constexpr float kMaxFerryPenalty = 8.0f * 3600.0f; // 8 hours

// Minimum and maximum average pedestrian speed (to validate input).
constexpr float kMinPedestrianSpeed = 0.5f;
constexpr float kMaxPedestrianSpeed = 25.0f;

// Crossing penalties. TODO - may want to lower stop impact when
// 2 cycleways or walkways cross
constexpr uint32_t kCrossingCosts[] = { 0, 0, 1, 1, 2, 3, 5, 15 };
}

// Maximum amount of seconds that will be allowed to be passed in to influence paths
// This can't be too high because sometimes a certain kind of path is required to be taken
constexpr float kMaxSeconds = 12.0f * kSecPerHour; // 12 hours

constexpr float kMaxFactor = 20.0f; // TODO - What should the maximum value of a factor be?

// Valid ranges and defaults
constexpr ranged_default_t<float> kMaxDistanceWheelchairRange{0, kMaxDistanceWheelchair, kMaxDistanceWheelchair};
constexpr ranged_default_t<float> kMaxDistanceFootRange{0, kMaxDistanceFoot, kMaxDistanceFoot};

constexpr ranged_default_t<float> kSpeedWheelchairRange{kMinPedestrianSpeed, kDefaultSpeedWheelchair, kMaxPedestrianSpeed};
constexpr ranged_default_t<float> kSpeedFootRange{kMinPedestrianSpeed, kDefaultSpeedFoot, kMaxPedestrianSpeed};

constexpr ranged_default_t<float> kStepPenaltyWheelchairRange{0, kDefaultStepPenaltyWheelchair, kMaxSeconds};
constexpr ranged_default_t<float> kStepPenaltyFootRange{0, kDefaultStepPenaltyFoot, kMaxSeconds};  

constexpr ranged_default_t<float> kMaxGradeWheelchairRange{0, kDefaultMaxGradeWheelchair, kMaxDistanceWheelchair};
constexpr ranged_default_t<float> kMaxGradeFootRange{0, kDefaultMaxGradeFoot, kMaxDistanceFoot};

// Other valid ranges and defaults (not dependent on type)
constexpr ranged_default_t<float> kModeWeightRange{0, kModeWeight, kMaxFactor};
constexpr ranged_default_t<float> kManeuverPenaltyRange{0, kDefaultManeuverPenalty, kMaxSeconds};
constexpr ranged_default_t<float> kGatePenaltyRange{0, kDefaultGatePenalty, kMaxSeconds};
constexpr ranged_default_t<float> kWalkwayFactorRange{0, kDefaultWalkwayFactor, kMaxFactor};
constexpr ranged_default_t<float> kSideWalkFactorRange{0, kDefaultSideWalkFactor, kMaxFactor};
constexpr ranged_default_t<float> kAlleyFactorRange{0, kDefaultAlleyFactor, kMaxFactor};
constexpr ranged_default_t<float> kDrivewayFactorRange{0, kDefaultDrivewayFactor, kMaxFactor};
constexpr ranged_default_t<float> kFerryCostRange{0, kDefaultFerryCost, kMaxSeconds};
constexpr ranged_default_t<float> kCountryCrossingCostRange{0, kDefaultCountryCrossingCost, kMaxSeconds};
constexpr ranged_default_t<float> kCountryCrossingPenaltyRange{0, kDefaultCountryCrossingPenalty, kMaxSeconds};
constexpr ranged_default_t<float> kUseFerryRange{0, kDefaultUseFerry, 1.0f};
constexpr ranged_default_t<float> kTransitStartEndMaxDistanceRange{0, kTransitStartEndMaxDistance,
                                                                     kTransitStartEndMaxDistance};
constexpr ranged_default_t<float> kTransitTransferMaxDistanceRange{0, kTransitTransferMaxDistance,
                                                                      kTransitTransferMaxDistance};

/**
 * Derived class providing dynamic edge costing for pedestrian routes.
 */
class PedestrianCost : public DynamicCost {
 public:
  /**
   * Constructor. Configuration / options for pedestrian costing are provided
   * via a property tree (JSON).
   * @param  pt  Property tree with configuration/options.
   */
  PedestrianCost(const boost::property_tree::ptree& pt);

  virtual ~PedestrianCost();

  /**
   * This method overrides the max_distance with the max_distance_mm per segment
   * distance. An example is a pure walking route may have a max distance of
   * 10000 meters (10km) but for a multi-modal route a lower limit of 5000
   * meters per segment (e.g. from origin to a transit stop or from the last
   * transit stop to the destination).
   */
  virtual void UseMaxMultiModalDistance();

  /**
   * Returns the maximum transfer distance between stops that you are willing
   * to travel for this mode.  In this case, it is the max walking
   * distance you are willing to walk between transfers.
   */
  virtual uint32_t GetMaxTransferDistanceMM();

  /**
   * This method overrides the weight for this mode.  The higher the value
   * the more the mode is favored.
   */
  virtual float GetModeWeight();

  /**
   * Get the access mode used by this costing method.
   * @return  Returns access mode.
   */
  uint32_t access_mode() const;

  /**
   * Checks if access is allowed for the provided directed edge.
   * This is generally based on mode of travel and the access modes
   * allowed on the edge. However, it can be extended to exclude access
   * based on other parameters.
   * @param  edge     Pointer to a directed edge.
   * @param  pred     Predecessor edge information.
   * @param  tile     current tile
   * @param  edgeid   edgeid that we care about
   * @return  Returns true if access is allowed, false if not.
   */
  virtual bool Allowed(const baldr::DirectedEdge* edge,
                       const EdgeLabel& pred,
                       const baldr::GraphTile*& tile,
                       const baldr::GraphId& edgeid) const;

  /**
   * Checks if access is allowed for an edge on the reverse path
   * (from destination towards origin). Both opposing edges are
   * provided.
   * @param  edge           Pointer to a directed edge.
   * @param  pred           Predecessor edge information.
   * @param  opp_edge       Pointer to the opposing directed edge.
   * @param  tile           Tile for the opposing edge (for looking
   *                        up restrictions).
   * @param  opp_edgeid     Opposing edge Id
   * @return  Returns true if access is allowed, false if not.
   */
  virtual bool AllowedReverse(const baldr::DirectedEdge* edge,
                 const EdgeLabel& pred,
                 const baldr::DirectedEdge* opp_edge,
                 const baldr::GraphTile*& tile,
                 const baldr::GraphId& opp_edgeid) const;

  /**
   * Checks if access is allowed for the provided node. Node access can
   * be restricted if bollards or gates are present.
   * @param  edge  Pointer to node information.
   * @return  Returns true if access is allowed, false if not.
   */
  virtual bool Allowed(const baldr::NodeInfo* node) const;

  /**
   * Get the cost to traverse the specified directed edge. Cost includes
   * the time (seconds) to traverse the edge.
   * @param   edge  Pointer to a directed edge.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost EdgeCost(const baldr::DirectedEdge* edge) const;

  /**
   * Returns the cost to make the transition from the predecessor edge.
   * Defaults to 0. Costing models that wish to include edge transition
   * costs (i.e., intersection/turn costs) must override this method.
   * @param  edge  Directed edge (the to edge)
   * @param  node  Node (intersection) where transition occurs.
   * @param  pred  Predecessor edge information.
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost TransitionCost(const baldr::DirectedEdge* edge,
                              const baldr::NodeInfo* node,
                              const EdgeLabel& pred) const;

  /**
   * Returns the cost to make the transition from the predecessor edge
   * when using a reverse search (from destination towards the origin).
   * Defaults to 0. Costing models that wish to include edge transition
   * costs (i.e., intersection/turn costs) must override this method.
   * @param  idx   Directed edge local index
   * @param  node  Node (intersection) where transition occurs.
   * @param  pred  the opposing current edge in the reverse tree.
   * @param  edge  the opposing predecessor in the reverse tree
   * @return  Returns the cost and time (seconds)
   */
  virtual Cost TransitionCostReverse(const uint32_t idx,
                                     const baldr::NodeInfo* node,
                                     const baldr::DirectedEdge* pred,
                                     const baldr::DirectedEdge* edge) const;

  /**
   * Get the cost factor for A* heuristics. This factor is multiplied
   * with the distance to the destination to produce an estimate of the
   * minimum cost to the destination. The A* heuristic must underestimate the
   * cost to the destination. So a time based estimate based on speed should
   * assume the maximum speed is used to the destination such that the time
   * estimate is less than the least possible time along roads.
   */
  virtual float AStarCostFactor() const;

  /**
   * Get the current travel type.
   * @return  Returns the current travel type.
   */
  virtual uint8_t travel_type() const;

  /**
   * Returns a function/functor to be used in location searching which will
   * exclude and allow ranking results from the search by looking at each
   * edges attribution and suitability for use as a location by the travel
   * mode used by the costing method. Function/functor is also used to filter
   * edges not usable / inaccessible by pedestrians.
   */
   virtual const EdgeFilter GetEdgeFilter() const {
     // Throw back a lambda that checks the access for this type of costing
     auto access_mask = access_mask_;
     return [access_mask](const baldr::DirectedEdge* edge) {
       return !(edge->trans_up() || edge->trans_down() || edge->is_shortcut() ||
           edge->use() >= Use::kRail ||
          !(edge->forwardaccess() & access_mask));
     };
   }

   virtual const NodeFilter GetNodeFilter() const {
     //throw back a lambda that checks the access for this type of costing
     auto access_mask = access_mask_;
     return [access_mask](const baldr::NodeInfo* node){
       return !(node->access() & access_mask);
     };
   }

  /**
   * Returns a function/functor to be used in location searching which will
   * exclude results from the search by looking at each node's attribution
   * @return Function/functor to be used in filtering out nodes
   */

 public:
  // Type: foot (default), wheelchair, etc.
  PedestrianType type_;

  uint32_t access_mask_;

  // Maximum pedestrian distance.
  uint32_t max_distance_;

  // This is the weight for this mode.  The higher the value the more the
  // mode is favored.
  float mode_weight_;

  // Maximum pedestrian distance in meters for multimodal routes.
  // Maximum distance at the beginning or end of a multimodal route
  // that you are willing to travel for this mode.  In this case,
  // it is the max walking distance.
  uint32_t transit_start_end_max_distance_;

  // Maximum transfer, distance in meters for multimodal routes.
  // Maximum transfer distance between stops that you are willing
  // to travel for this mode.  In this case, it is the max distance
  // you are willing to walk between transfers.
  uint32_t transit_transfer_max_distance_;

  // Minimal surface type usable by the pedestrian type
  Surface minimal_allowed_surface_;

  uint32_t max_grade_;    // Maximum grade (percent).
  float speed_;           // Pedestrian speed.
  float speedfactor_;     // Speed factor for costing. Based on speed.
  float walkway_factor_;  // Factor for favoring walkways and paths.
  float sidewalk_factor_; // Factor for favoring sidewalks.
  float alley_factor_;    // Avoid alleys factor.
  float driveway_factor_; // Avoid driveways factor.
  float step_penalty_;    // Penalty applied to steps/stairs (seconds).
  float gate_penalty_;    // Penalty (seconds) to go through gate
  float maneuver_penalty_;          // Penalty (seconds) when inconsistent names
  float country_crossing_cost_;     // Cost (seconds) to go through toll booth
  float country_crossing_penalty_;  // Penalty (seconds) to go across a country border
  float ferry_cost_;                // Cost (seconds) to exit a ferry
  float ferry_penalty_;             // Penalty (seconds) to enter a ferry
  float ferry_weight_;              // Weighting to apply to ferry edges
  float use_ferry_;
};

// Constructor. Parse pedestrian options from property tree. If option is
// not present, set the default.
PedestrianCost::PedestrianCost(const boost::property_tree::ptree& pt)
    : DynamicCost(pt, TravelMode::kPedestrian) {
  // Set hierarchy to allow unlimited transitions
  for (auto& h : hierarchy_limits_) {
    h.max_up_transitions = kUnlimitedTransitions;
  }

  allow_transit_connections_ = false;

  // Get the pedestrian type - enter as string and convert to enum
  std::string type = pt.get<std::string>("type", "foot");
  if (type == "wheelchair") {
    type_ = PedestrianType::kWheelchair;
  } else if (type == "segway") {
    type_ = PedestrianType::kSegway;
  } else {
    type_ = PedestrianType::kFoot;
  }

  // Set type specific defaults, override with URL inputs
  if (type == "wheelchair") {
    access_mask_ = kWheelchairAccess;
    max_distance_  = pt.get<uint32_t>("max_distance", kMaxDistanceWheelchair);
    speed_ = pt.get<float>("walking_speed", kDefaultSpeedWheelchair);
    step_penalty_  = pt.get<float>("step_penalty", kDefaultStepPenaltyWheelchair);
    max_grade_ = pt.get<float>("max_grade", kDefaultMaxGradeWheelchair);
    minimal_allowed_surface_ = Surface::kCompacted;
  } else {
    // Assume type = foot
    access_mask_ = kPedestrianAccess;
    max_distance_  = pt.get<uint32_t>("max_distance", kMaxDistanceFoot);
    speed_ = pt.get<float>("walking_speed", kDefaultSpeedFoot);
    step_penalty_  = pt.get<float>("step_penalty", kDefaultStepPenaltyFoot);
    max_grade_ = pt.get<float>("max_grade", kDefaultMaxGradeFoot);
    minimal_allowed_surface_ = Surface::kPath;
  }


  mode_weight_                    = pt.get<float>("mode_weight", kModeWeight);
  maneuver_penalty_               = pt.get<float>("maneuver_penalty",
                                                    kDefaultManeuverPenalty);
  gate_penalty_                   = pt.get<float>("gate_penalty", kDefaultGatePenalty);
  walkway_factor_ = pt.get<float>("walkway_factor", kDefaultWalkwayFactor);
  sidewalk_factor_ = pt.get<float>("sidewalk_factor", kDefaultSideWalkFactor);
  alley_factor_                   = pt.get<float>("alley_factor", kDefaultAlleyFactor);
  driveway_factor_                = pt.get<float>("driveway_factor", kDefaultDrivewayFactor);
  ferry_cost_ = pt.get<float>("ferry_cost", kDefaultFerryCost);
  country_crossing_cost_          = pt.get<float>("country_crossing_cost",
                                                  kDefaultCountryCrossingCost);
  country_crossing_penalty_       = pt.get<float>("country_crossing_penalty",
                                                  kDefaultCountryCrossingPenalty);
  transit_start_end_max_distance_ = pt.get<uint32_t>("transit_start_end_max_distance",
                                                     kTransitStartEndMaxDistance);
  transit_transfer_max_distance_  = pt.get<uint32_t>("transit_transfer_max_distance",
                                                     kTransitTransferMaxDistance);

  // Modify ferry penalty and edge weighting based on use_ferry_ factor
  use_ferry_ = pt.get<float>("use_ferry", kDefaultUseFerry);
  if (use_ferry_ < 0.5f) {
    // Penalty goes from max at use_ferry_ = 0 to 0 at use_ferry_ = 0.5
    ferry_penalty_ = static_cast<uint32_t>(kMaxFerryPenalty * (1.0f - use_ferry_ * 2.0f));

    // Cost X10 at use_ferry_ == 0, slopes downwards towards 1.0 at use_ferry_ = 0.5
    ferry_weight_ = 10.0f - use_ferry_ * 18.0f;
  } else {
    // Add a ferry weighting factor to influence cost along ferries to make
    // them more favorable if desired rather than driving. No ferry penalty.
    // Half the cost at use_ferry_ == 1, progress to 1.0 at use_ferry_ = 0.5
    ferry_penalty_ = 0.0f;
    ferry_weight_  = 1.5f - use_ferry_;
  }

  // Set the speed factor (to avoid division in costing)
  speedfactor_ = (kSecPerHour * 0.001f) / speed_;
}

// Destructor
PedestrianCost::~PedestrianCost() {
}

// This method overrides the max_distance with the max_distance_mm per segment
// distance. An example is a pure walking route may have a max distance of
// 10000 meters (10km) but for a multi-modal route a lower limit of 5000
// meters per segment (e.g. from origin to a transit stop or from the last
// transit stop to the destination).
void PedestrianCost::UseMaxMultiModalDistance() {
  max_distance_ = transit_start_end_max_distance_;
}

// Returns the maximum transfer distance between stops that you are willing
// to travel for this mode.  In this case, it is the max walking
// distance you are willing to walk between transfers.
uint32_t PedestrianCost::GetMaxTransferDistanceMM() {
  return transit_transfer_max_distance_;
}

// This method overrides the weight for this mode.  The higher the value
// the more the mode is favored.
float PedestrianCost::GetModeWeight() {
  return mode_weight_;
}

// Get the access mode used by this costing method.
uint32_t PedestrianCost::access_mode() const {
  return access_mask_;
}

// Check if access is allowed on the specified edge. Disallow if no
// access for this pedestrian type, if surface type exceeds (worse than)
// the minimum allowed surface type, or if max grade is exceeded.
// Disallow edges where max. distance will be exceeded.
bool PedestrianCost::Allowed(const baldr::DirectedEdge* edge,
                             const EdgeLabel& pred,
                             const baldr::GraphTile*& tile,
                             const baldr::GraphId& edgeid) const {
  // TODO - obtain and check the access restrictions.

  if (!(edge->forwardaccess() & access_mask_) ||
       (edge->surface() > minimal_allowed_surface_) ||
        edge->is_shortcut() || IsUserAvoidEdge(edgeid) ||
 //      (edge->max_up_slope() > max_grade_ || edge->max_down_slope() > max_grade_) ||
      ((pred.path_distance() + edge->length()) > max_distance_)) {
    return false;
  }

  // Disallow transit connections (except when set for multi-modal routes)
  if (!allow_transit_connections_ && edge->use() == Use::kTransitConnection) {
    return false;
  }
  return true;
}

// Checks if access is allowed for an edge on the reverse path (from
// destination towards origin). Both opposing edges are provided.
bool PedestrianCost::AllowedReverse(const baldr::DirectedEdge* edge,
               const EdgeLabel& pred,
               const baldr::DirectedEdge* opp_edge,
               const baldr::GraphTile*& tile,
               const baldr::GraphId& opp_edgeid) const {
  // TODO - obtain and check the access restrictions.

  // Do not check max walking distance and assume we are not allowing
  // transit connections. Assume this method is never used in
  // multimodal routes).
  if (!(opp_edge->forwardaccess() & access_mask_) ||
       (opp_edge->surface() > minimal_allowed_surface_) ||
        opp_edge->is_shortcut() || IsUserAvoidEdge(opp_edgeid) ||
 //      (opp_edge->max_up_slope() > max_grade_ || opp_edge->max_down_slope() > max_grade_) ||
        opp_edge->use() == Use::kTransitConnection) {
    return false;
  }
  return true;
}

// Check if access is allowed at the specified node.
bool PedestrianCost::Allowed(const baldr::NodeInfo* node) const {
  return (node->access() & access_mask_);
}

// Returns the cost to traverse the edge and an estimate of the actual time
// (in seconds) to traverse the edge.
Cost PedestrianCost::EdgeCost(const baldr::DirectedEdge* edge) const {

  // Ferries are a special case - they use the ferry speed (stored on the edge)
  if (edge->use() == Use::kFerry) {
    float sec = edge->length() * (kSecPerHour * 0.001f) /
            static_cast<float>(edge->speed());
    return { sec * ferry_weight_, sec };
  }

  // Slightly favor walkways/paths and penalize alleys and driveways.
  float sec = edge->length() * speedfactor_;
  if (edge->use() == Use::kFootway) {
    return { sec * walkway_factor_, sec };
  } else if (edge->use() == Use::kAlley) {
    return { sec * alley_factor_, sec };
  } else if (edge->use() == Use::kDriveway) {
    return { sec * driveway_factor_, sec };
  } else if (edge->use() == Use::kSidewalk) {
    return { sec * sidewalk_factor_, sec };
  } else if (edge->roundabout()) {
    return { sec * kRoundaboutFactor, sec };
  } else {
    return { sec, sec };
  }
}

// Returns the time (in seconds) to make the transition from the predecessor
Cost PedestrianCost::TransitionCost(const baldr::DirectedEdge* edge,
                                    const baldr::NodeInfo* node,
                                    const EdgeLabel& pred) const {
  // Special cases: fixed penalty for steps/stairs
  if (edge->use() == Use::kSteps) {
    return { step_penalty_, 0.0f };
  }

  // Penalty through gates and border control.
  float seconds = 0.0f;
  float penalty = 0.0f;
  if (node->type() == NodeType::kBorderControl) {
    seconds += country_crossing_cost_;
    penalty += country_crossing_penalty_;
  } else if (node->type() == NodeType::kGate) {
    penalty += gate_penalty_;
  }

  if ((pred.use() != Use::kFerry && edge->use() == Use::kFerry)) {
    seconds += ferry_cost_;
    penalty += ferry_penalty_;
  }

  uint32_t idx = pred.opp_local_idx();
  // Ignore name inconsistency when entering a link to avoid double penalizing.
  if (!edge->link() && !node->name_consistency(idx, edge->localedgeidx())) {
    // Slight maneuver penalty
    penalty += maneuver_penalty_;
  }

  // Costs for crossing an intersection.
  if (edge->edge_to_right(idx) && edge->edge_to_left(idx)) {
    seconds += kCrossingCosts[edge->stopimpact(idx)];
  }
  return { seconds + penalty, seconds };
}

// Returns the cost to make the transition from the predecessor edge
// when using a reverse search (from destination towards the origin).
// Defaults to 0. Costing models that wish to include edge transition
// costs (i.e., intersection/turn costs) must override this method.
Cost PedestrianCost::TransitionCostReverse(
    const uint32_t idx, const baldr::NodeInfo* node,
    const baldr::DirectedEdge* pred, const baldr::DirectedEdge* edge) const {
  // Special cases: fixed penalty for steps/stairs
  if (edge->use() == Use::kSteps) {
    return { step_penalty_, 0.0f };
  }

  // Penalty through gates and border control.
  float seconds = 0.0f;
  float penalty = 0.0f;
  if (node->type() == NodeType::kBorderControl) {
    seconds += country_crossing_cost_;
    penalty += country_crossing_penalty_;
  } else if (node->type() == NodeType::kGate) {
    penalty += gate_penalty_;
  }

  if (pred->use() != Use::kFerry && edge->use() == Use::kFerry) {
    seconds += ferry_cost_;
    penalty += ferry_penalty_;
  }

  // Ignore name inconsistency when entering a link to avoid double penalizing.
  if (!edge->link() && !node->name_consistency(idx, edge->localedgeidx())) {
    // Slight maneuver penalty
    penalty += maneuver_penalty_;
  }

  // Costs for crossing an intersection.
  if (edge->edge_to_right(idx) && edge->edge_to_left(idx)) {
    seconds += kCrossingCosts[edge->stopimpact(idx)];
  }
  return { seconds + penalty, seconds };
}

// Get the cost factor for A* heuristics. This factor is multiplied
// with the distance to the destination to produce an estimate of the
// minimum cost to the destination. The A* heuristic must underestimate the
// cost to the destination. So a time based estimate based on speed should
// assume the maximum speed is used to the destination such that the time
// estimate is less than the least possible time along roads.
float PedestrianCost::AStarCostFactor() const {
  // Use the factor to favor walkways/paths if < 1.0f
  return (walkway_factor_ < 1.0f) ? walkway_factor_ * speedfactor_ : speedfactor_;
}

// Returns the current travel type.
uint8_t PedestrianCost::travel_type() const {
  return static_cast<uint8_t>(type_);
}

cost_ptr_t CreatePedestrianCost(const boost::property_tree::ptree& config) {
  return std::make_shared<PedestrianCost>(config);
}

}
}

/**********************************************************************************************/

#ifdef INLINE_TEST

using namespace valhalla;
using namespace sif;

namespace {

PedestrianCost* make_pedestriancost_from_json(const std::string& property, float testVal, const std::string& type) {
  std::stringstream ss;
  ss << R"({")" << property << R"(":)" << testVal << R"(,"type":")" << type << R"(")" << "}";
  boost::property_tree::ptree costing_ptree;
  boost::property_tree::read_json(ss, costing_ptree);
  return new PedestrianCost(costing_ptree);
}

std::uniform_real_distribution<float>* make_distributor_from_range (const ranged_default_t<float>& range) {
  float rangeLength = range.max - range.min;
  return new std::uniform_real_distribution<float>(range.min - rangeLength, range.max + rangeLength);
}

void testPedestrianCostParams() {
  constexpr unsigned seed = 0;
  std::default_random_engine generator(seed);
  std::shared_ptr<std::uniform_real_distribution<float>> distributor;
  std::shared_ptr<PedestrianCost> ctorTester;

  // Wheelchair tests
  // max_distance_
  distributor.reset(make_distributor_from_range(kMaxDistanceWheelchairRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("max_distance", (*distributor)(generator), "wheelchair"));
    if (ctorTester->max_distance_ < kMaxDistanceWheelchairRange.min ||
        ctorTester->max_distance_ > kMaxDistanceWheelchairRange.max) {
      throw std::runtime_error ("max_distance_ with type wheelchair is not within it's range");
    }
  }

  // speed_
  distributor.reset(make_distributor_from_range(kSpeedWheelchairRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("walking_speed", (*distributor)(generator), "wheelchair"));
    if (ctorTester->speed_ < kSpeedWheelchairRange.min ||
        ctorTester->speed_ > kSpeedWheelchairRange.max) {
      throw std::runtime_error ("speed_ with type wheelchair is not within it's range");
    }
  }

  // step_penalty_
  distributor.reset(make_distributor_from_range(kStepPenaltyWheelchairRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("step_penalty", (*distributor)(generator), "wheelchair"));
    if (ctorTester->step_penalty_ < kStepPenaltyWheelchairRange.min ||
        ctorTester->step_penalty_ > kStepPenaltyWheelchairRange.max) {
      throw std::runtime_error ("step_penalty_ with type wheelchair is not within it's range");
    }
  }

  // max_grade_
  distributor.reset(make_distributor_from_range(kMaxGradeWheelchairRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("max_grade", (*distributor)(generator), "wheelchair"));
    if (ctorTester->max_grade_ < kMaxGradeWheelchairRange.min ||
        ctorTester->max_grade_ > kMaxGradeWheelchairRange.max) {
      throw std::runtime_error ("max_grade_ with type wheelchair is not within it's range");
    }
  }


  // Foot tests
  // max_distance_
  distributor.reset(make_distributor_from_range(kMaxDistanceFootRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("max_distance", (*distributor)(generator), "foot"));
    if (ctorTester->max_distance_ < kMaxDistanceFootRange.min ||
        ctorTester->max_distance_ > kMaxDistanceFootRange.max) {
      throw std::runtime_error ("max_distance_ with type foot is not within it's range");
    }
  }

  // speed_
  distributor.reset(make_distributor_from_range(kSpeedFootRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("walking_speed", (*distributor)(generator), "foot"));
    if (ctorTester->speed_ < kSpeedFootRange.min ||
        ctorTester->speed_ > kSpeedFootRange.max) {
      throw std::runtime_error ("speed_ with type foot is not within it's range");
    }
  }

  // step_penalty_
  distributor.reset(make_distributor_from_range(kStepPenaltyFootRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("step_penalty_", (*distributor)(generator), "foot"));
    if (ctorTester->step_penalty_ < kStepPenaltyFootRange.min ||
        ctorTester->step_penalty_ > kStepPenaltyFootRange.max) {
      throw std::runtime_error ("step_penalty_ with type foot is not within it's range");
    }
  }

  // max_grade_
  distributor.reset(make_distributor_from_range(kMaxGradeFootRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("max_grade_", (*distributor)(generator), "foot"));
    if (ctorTester->max_grade_ < kMaxGradeFootRange.min ||
        ctorTester->max_grade_ > kMaxGradeFootRange.max) {
      throw std::runtime_error ("max_grade_ with type foot is not within it's range");
    }
  }


  // Non type dependent tests
  // mode_weight_
    distributor.reset(make_distributor_from_range(kModeWeightRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("mode_weight", (*distributor)(generator), "foot"));
    if (ctorTester->mode_weight_ < kModeWeightRange.min ||
        ctorTester->mode_weight_ > kModeWeightRange.max) {
      throw std::runtime_error ("mode_weight_ is not within it's range");
    }
  }

  // maneuver_penalty_
  distributor.reset(make_distributor_from_range(kManeuverPenaltyRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("maneuver_penalty", (*distributor)(generator), "foot"));
    if (ctorTester->maneuver_penalty_ < kManeuverPenaltyRange.min ||
        ctorTester->maneuver_penalty_ > kManeuverPenaltyRange.max) {
      throw std::runtime_error ("maneuver_penalty_ is not within it's range");
    }
  }

  // gate_penalty_
  distributor.reset(make_distributor_from_range(kGatePenaltyRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("gate_penalty", (*distributor)(generator), "foot"));
    if (ctorTester->gate_penalty_ < kGatePenaltyRange.min ||
        ctorTester->gate_penalty_ > kGatePenaltyRange.max) {
      throw std::runtime_error ("gate_penalty_ is not within it's range");
    }
  }

  // walkway_factor_
  distributor.reset(make_distributor_from_range(kWalkwayFactorRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("walkway_factor", (*distributor)(generator), "foot"));
    if (ctorTester->walkway_factor_ < kWalkwayFactorRange.min ||
        ctorTester->walkway_factor_ > kWalkwayFactorRange.max) {
      throw std::runtime_error ("walkway_factor_ is not within it's range");
    }
  }

  // sidewalk_factor_
  distributor.reset(make_distributor_from_range(kSideWalkFactorRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("sidewalk_factor", (*distributor)(generator), "foot"));
    if (ctorTester->sidewalk_factor_ < kSideWalkFactorRange.min ||
        ctorTester->sidewalk_factor_ > kSideWalkFactorRange.max) {
      throw std::runtime_error ("sidewalk_factor_ is not within it's range");
    }
  }

  // alley_factor_
  distributor.reset(make_distributor_from_range(kAlleyFactorRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("alley_factor", (*distributor)(generator), "foot"));
    if (ctorTester->alley_factor_ < kAlleyFactorRange.min ||
        ctorTester->alley_factor_ > kAlleyFactorRange.max) {
      throw std::runtime_error ("alley_factor_ is not within it's range");
    }
  }

  // driveway_factor_
  distributor.reset(make_distributor_from_range(kDrivewayFactorRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("driveway_factor", (*distributor)(generator), "foot"));
    if (ctorTester->driveway_factor_ < kDrivewayFactorRange.min ||
        ctorTester->driveway_factor_ > kDrivewayFactorRange.max) {
      throw std::runtime_error ("driveway_factor_ is not within it's range");
    }
  }

  // ferry_cost_
  distributor.reset(make_distributor_from_range(kFerryCostRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("ferry_cost", (*distributor)(generator), "foot"));
    if (ctorTester->ferry_cost_ < kFerryCostRange.min ||
        ctorTester->ferry_cost_ > kFerryCostRange.max) {
      throw std::runtime_error ("ferry_cost_ is not within it's range");
    }
  }

  // country_crossing_cost_
  distributor.reset(make_distributor_from_range(kCountryCrossingCostRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("country_crossing_cost", (*distributor)(generator), "foot"));
    if (ctorTester->country_crossing_cost_ < kCountryCrossingCostRange.min ||
        ctorTester->country_crossing_cost_ > kCountryCrossingCostRange.max) {
      throw std::runtime_error ("country_crossing_cost_ is not within it's range");
    }
  }

  // country_crossing_penalty_
  distributor.reset(make_distributor_from_range(kCountryCrossingPenaltyRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("country_crossing_penalty", (*distributor)(generator), "foot"));
    if (ctorTester->country_crossing_penalty_ < kCountryCrossingPenaltyRange.min ||
        ctorTester->country_crossing_penalty_ > kCountryCrossingPenaltyRange.max) {
      throw std::runtime_error ("country_crossing_penalty_ is not within it's range");
    }
  }

  // use_ferry_
  distributor.reset(make_distributor_from_range(kUseFerryRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("use_ferry", (*distributor)(generator), "foot"));
    if (ctorTester->use_ferry_ < kUseFerryRange.min ||
        ctorTester->use_ferry_ > kUseFerryRange.max) {
      throw std::runtime_error ("use_ferry_ is not within it's range");
    }
  }

  // transit_start_end_max_distance_
  distributor.reset(make_distributor_from_range(kTransitStartEndMaxDistanceRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("transit_start_end_max_distance",
                                                  (*distributor)(generator), "foot"));
    if (ctorTester->transit_start_end_max_distance_ < kTransitStartEndMaxDistanceRange.min ||
        ctorTester->transit_start_end_max_distance_ > kTransitStartEndMaxDistanceRange.max) {
      throw std::runtime_error ("transit_start_end_max_distance_ is not within it's range");
    }
  }

  // transit_transfer_max_distance_
  distributor.reset(make_distributor_from_range(kTransitTransferMaxDistanceRange));
  for (unsigned i = 0; i < 100; ++i) {
    ctorTester.reset(make_pedestriancost_from_json("transit_transfer_max_distance",
                                                  (*distributor)(generator), "foot"));
    if (ctorTester->transit_transfer_max_distance_ < kTransitTransferMaxDistanceRange.min ||
        ctorTester->transit_transfer_max_distance_ > kTransitTransferMaxDistanceRange.max) {
      throw std::runtime_error ("transit_transfer_max_distance_ is not within it's range");
    }
  }
}
}

int main() {
  test::suite suite("costing");

  suite.test(TEST_CASE(testPedestrianCostParams));

  return suite.tear_down();
}

#endif
