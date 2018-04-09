#include <queue>
#include "em_exploration/SLAM2D.h"
#include "em_exploration/Utils.h"

using namespace gtsam;

namespace em_exploration {

std::ostream& operator<<(std::ostream &os, const SparseBlockVector &vector) {
  for (auto it = vector.begin(); it != vector.end(); ++it)
    os << it->first << std::endl << it->second << std::endl;
  return os;
}

std::size_t hash_value(const KeyPair &key_pair) {
  std::size_t seed = 0;
  boost::hash_combine(seed, key_pair.first);
  boost::hash_combine(seed, key_pair.second);
  return seed;
}

Matrix FastMarginals::jointMarginalCovariance(const std::vector<Key> &variables) {
  size_t dim = 0;
  std::vector<size_t> variable_acc_dim;
  for (Key key : variables) {
    variable_acc_dim.push_back(dim);
    dim += getKeyDim(key);
  }

  std::vector<int> variable_idx(variables.size());
  std::iota(variable_idx.begin(), variable_idx.end(), 0);
  std::sort(variable_idx.begin(), variable_idx.end(),
            [this, &variables](int i, int j) {
              return key_idx_[variables[i]] < key_idx_[variables[j]];
            });

  Matrix cov = Matrix::Zero(dim, dim);
  for (int j = variable_idx.size() - 1; j >= 0; --j) {
    Key key_j = variables[variable_idx[j]];
    size_t col = variable_acc_dim[variable_idx[j]];

    for (int i = j; i >= 0; --i) {
      Key key_i = variables[variable_idx[i]];
      size_t row = variable_acc_dim[variable_idx[i]];

      if (row > col) {
        cov.block(col, row, getKeyDim(key_j), getKeyDim(key_i)) = recover(key_i, key_j).transpose();
      } else {
        cov.block(row, col, getKeyDim(key_i), getKeyDim(key_j)) = recover(key_i, key_j);
      }
    }
  }
  cov.triangularView<Eigen::Lower>() = cov.transpose();
  return cov;
}

Matrix FastMarginals::getRBlock(const Key &key_i, const Key &key_j) {
  ISAM2Clique::shared_ptr clique = (*isam2_)[key_i];
  const ISAM2Clique::sharedConditional conditional = clique->conditional();
  if (conditional->find(key_j) == conditional->end())
    return Matrix();

  size_t block_row = conditional->find(key_i) - conditional->begin();
  size_t block_col = conditional->find(key_j) - conditional->begin();
  const auto &m = conditional->matrixObject();
  DenseIndex row = m.offset(block_row);
  DenseIndex col = m.offset(block_col);
  return m.matrix().block(row, col, getKeyDim(key_i), getKeyDim(key_j));
}

const SparseBlockVector& FastMarginals::getRRow(const Key &key) {
  const auto &it = cov_cache_.rows.find(key);
  if (it == cov_cache_.rows.cend()) {
    auto ret = cov_cache_.rows.insert(std::make_pair(key, SparseBlockVector()));
    bool started = false;
    for (Key key_i : ordering_) {
      if (key_i == key)
        started = true;
      if (!started)
        continue;

      Matrix block = getRBlock(key, key_i);
      if (block.size() > 0)
        ret.first->second.insert(key_i, block);
    }
    return ret.first->second;
  } else
    return it->second;
}

Matrix FastMarginals::getR(const std::vector<Key> &variables) {
  size_t dim = 0;
  for (Key key : variables)
    dim += getKeyDim(key);

  Matrix R = Matrix::Zero(dim, dim);
  size_t row = 0;
  for (size_t i = 0; i < variables.size(); ++i) {
    Key key_i = variables[i];
    size_t col = row;
    size_t dim_i = getKeyDim(key_i);
    for (size_t j = i; j < variables.size(); ++j) {
      Key key_j = variables[j];
      size_t dim_j = getKeyDim(key_j);
      Matrix block = getRBlock(key_i, key_j);
      if (block.size() > 0)
        R.block(row, col, dim_i, dim_j) = block;
      col += dim_j;
    }
    row += dim_i;
  }
  return R;
}

size_t FastMarginals::getKeyDim(const Key &key) {
  return isam2_->getLinearizationPoint().at(key).dim();
}

Matrix FastMarginals::getKeyDiag(const Key &key) {
  auto it = cov_cache_.diag.find(key);
  if (it == cov_cache_.diag.end()) {
    auto ret = cov_cache_.diag.insert(std::make_pair(key, getRBlock(key, key).inverse()));
    return ret.first->second;
  } else
    return it->second;
}

void FastMarginals::initialize() {
  std::queue<ISAM2Clique::shared_ptr> q;
  assert(isam2_->roots().size() == 1);
  q.push(isam2_->roots()[0]);
  while (!q.empty()) {
    ISAM2Clique::shared_ptr c = q.front();
    q.pop();
    std::vector<Key> sub;
    assert(c->conditional() != nullptr);
    for (Key key : c->conditional()->frontals()) {
      sub.push_back(key);
    }
    ordering_.insert(ordering_.begin(), sub.begin(), sub.end());
    for (auto child : c->children)
      q.push(child);
  }

  size_t dim = 0;
//    for (Key key : ordering_) {
  for (size_t i = 0; i < ordering_.size(); ++i) {
    Key key = ordering_[i];
    key_idx_[key] = i;
  }
}

Matrix FastMarginals::sumJ(const Key key_l, const Key key_i) {
  Matrix sum = Matrix::Zero(getKeyDim(key_i), getKeyDim(key_l));
  const SparseBlockVector &Ri = getRRow(key_i);

  size_t idx_l = key_idx_[key_l];
  size_t idx_i = key_idx_[key_i];
  for (auto it = Ri.begin(); it != Ri.end(); ++it) {
    Key key_j = it->first;
    size_t idx_j = key_idx_[key_j];
    if (idx_j > idx_i) {
      sum += it->second * (idx_j > idx_l ? recover(key_l, key_j).transpose() : recover(key_j, key_l));
    }
  }
  return sum;
}

Matrix FastMarginals::recover(const Key &key_i, const Key &key_l) {
  KeyPair key_pair = std::make_pair(key_i, key_l);
  auto entry_iter = cov_cache_.entries.find(key_pair);
  if (entry_iter == cov_cache_.entries.end()) {
    Matrix res;
    if (key_i == key_l)
      res = getKeyDiag(key_l) * (getKeyDiag(key_l).transpose() - sumJ(key_l, key_l));
    else
      res = -getKeyDiag(key_i) * sumJ(key_l, key_i);

    cov_cache_.entries.insert(std::make_pair(key_pair, res));
    return res;
  } else {
    return entry_iter->second;
  }
}

SLAM2D::SLAM2D(const Map::Parameter &parameter, RNG::SeedType seed)
    : map_(parameter), step_(0), marginals_update_(false), optimized_(false), rng_(seed) {
  ISAM2Params params;
//    params.setFactorization("QR");
  isam_ = std::make_shared<ISAM2>(params);
}

void SLAM2D::printParameters() const {
  std::cout << "----------------------------" << std::endl;
  std::cout << "SLAM2D:" << std::endl;
  map_.getParameter().print();
  std::cout << "----------------------------" << std::endl;
}

void SLAM2D::fromISAM2(std::shared_ptr<gtsam::ISAM2> isam, const Map &map, const gtsam::Values values) {
  isam_ = isam;
  result_ = values;
  optimized_ = true;
  marginals_update_ = false;

  map_ = map;
}

void SLAM2D::addPrior(unsigned int key, const LandmarkBeliefState &landmark_state) {
  optimized_ = false;

  Symbol l0 = getLandmarkSymbol(key);

  noiseModel::Gaussian::shared_ptr noise_model =
      noiseModel::Gaussian::Information(landmark_state.information);
  graph_.add(PriorFactor<Point2>(l0, landmark_state.point, noise_model));

  initial_estimate_.insert(l0, landmark_state.point);
}

void SLAM2D::addPrior(const VehicleBeliefState &vehicle_state) {
  assert(step_ == 0);

  optimized_ = false;

  Symbol x0 = getVehicleSymbol(step_++);

  noiseModel::Gaussian::shared_ptr noise_model =
      noiseModel::Gaussian::Information(vehicle_state.information);
  graph_.add(PriorFactor<Pose2>(x0, vehicle_state.pose, noise_model));

  initial_estimate_.insert(x0, vehicle_state.pose);
}

SLAM2D::OdometryFactor2DPtr SLAM2D::buildOdometryFactor(unsigned int x1, unsigned int x2,
                                                        const SimpleControlModel::ControlState &control_state) {
  Symbol sx1 = getVehicleSymbol(x1);
  Symbol sx2 = getVehicleSymbol(x2);

  noiseModel::Diagonal::shared_ptr noise_model =
      noiseModel::Diagonal::Sigmas(control_state.getSigmas());
  return boost::make_shared<SLAM2D::OdometryFactor2D>(sx1, sx2,
                                                      control_state.getOdom(), noise_model);
}

void SLAM2D::addOdometry(const SimpleControlModel::ControlState &control_state) {
  optimized_ = false;

  OdometryFactor2DPtr factor = SLAM2D::buildOdometryFactor(step_ - 1, step_, control_state);
  graph_.add(*factor);

  Symbol x1 = getVehicleSymbol(step_ - 1);
  Symbol x2 = getVehicleSymbol(step_++);

  Pose2 p1;
  if (result_.exists(x1)) {
    p1 = result_.at<Pose2>(x1);
  } else {
    p1 = initial_estimate_.at<Pose2>(x1);
  }

  Pose2 p2 = p1 * control_state.getOdom();
  initial_estimate_.insert(x2, p2);
}

SLAM2D::MeasurementFactor2DPtr SLAM2D::buildMeasurementFactor(unsigned int x, unsigned int l,
                                                              const BearingRangeSensorModel::Measurement &measurement) {
  Symbol sx = getVehicleSymbol(x);
  Symbol sl = getLandmarkSymbol(l);

  noiseModel::Diagonal::shared_ptr noise_model =
      noiseModel::Diagonal::Sigmas(measurement.getSigmas());
  return boost::make_shared<MeasurementFactor2D>(sx, sl,
                                                 measurement.getBearing(),
                                                 measurement.getRange(), noise_model);
}

void SLAM2D::addMeasurement(unsigned int key, const BearingRangeSensorModel::Measurement &measurement) {
  optimized_ = false;

  SLAM2D::MeasurementFactor2DPtr factor = SLAM2D::buildMeasurementFactor(step_ - 1, key, measurement);
  graph_.add(*factor);

  Symbol x = getVehicleSymbol(step_ - 1);
  Symbol l = getLandmarkSymbol(key);

  if (!initial_estimate_.exists(l) && !result_.exists(l)) {
    Pose2 origin;
    if (result_.exists(x))
      origin = result_.at<Pose2>(x);
    else
      origin = initial_estimate_.at<Pose2>(x);

    Point2 global = measurement.transformFrom(origin);
    initial_estimate_.insert(l, global);

    map_.addLandmark(key, global);
  }
}

void SLAM2D::saveGraph(const std::string &name) const {
  std::ofstream os(name);
  if (!os.is_open()) return;

  isam_->getFactorsUnsafe().saveGraph(os, result_);
  std::cout << "Visualize the graph with `dot -Tpdf ./graph.dot -O`" << std::endl;
  os.close();
}

void SLAM2D::printGraph() const {
  graph_.print("Graph:\n");
  initial_estimate_.print("Initial Estimate:\n");
}

Eigen::MatrixXd SLAM2D::jointMarginalCovarianceLocal(const std::vector<unsigned int> &poses,
                                                     const std::vector<unsigned int> &landmarks) const {
  if (!marginals_update_) {
#ifdef USE_FAST_MARGINAL
    marginals_ = std::make_shared<FastMarginals>(isam_);
#else
    marginals_ = std::make_shared<Marginals>(isam_->getFactorsUnsafe(), result_);
#endif
    marginals_update_ = true;
  }

  std::vector<Key> keys;
  for (unsigned int x : poses)
    keys.emplace_back(getVehicleSymbol(x));
  for (unsigned int l : landmarks)
    keys.emplace_back(getLandmarkSymbol(l));

#ifdef USE_FAST_MARGINAL
  return marginals_->jointMarginalCovariance(keys);
#else
  JointMarginal cov = marginals_->jointMarginalCovariance(keys);

  size_t n = poses.size() * 3 + landmarks.size() * 2;
  Matrix S(n, n);
  int p = 0, q = 0;
  for (int i = 0; i < keys.size(); ++i) {
    for (int j = i; j < keys.size(); ++j) {
      Matrix Sij = cov.at(keys[i], keys[j]);
      S.block(p, q, Sij.rows(), Sij.cols()) = Sij;
      q += Sij.cols();
      if (j == keys.size() - 1) {
        p += Sij.rows();
        q = p;
      }
    }
  }
  S.triangularView<Eigen::Lower>() = S.transpose();
  return S;
#endif
}

Eigen::MatrixXd SLAM2D::jointMarginalCovariance(const std::vector<unsigned int> &poses,
                                                const std::vector<unsigned int> &landmarks) const {
  if (!marginals_update_) {
#ifdef USE_FAST_MARGINAL
    marginals_ = std::make_shared<FastMarginals>(isam_);
#else
    marginals_ = std::make_shared<Marginals>(isam_->getFactorsUnsafe(), result_);
#endif
    marginals_update_ = true;
  }

  std::vector<Key> keys;
  for (unsigned int x : poses)
    keys.emplace_back(getVehicleSymbol(x));
  for (unsigned int l : landmarks)
    keys.emplace_back(getLandmarkSymbol(l));

#ifdef USE_FAST_MARGINAL
  /// TODO
  assert(false);
#else
  JointMarginal cov = marginals_->jointMarginalCovariance(keys);

  size_t n = poses.size() * 3 + landmarks.size() * 2;
  Matrix S(n, n);
  int p = 0, q = 0;
  for (int i = 0; i < keys.size(); ++i) {
    for (int j = i; j < keys.size(); ++j) {
      Matrix Sij = cov.at(keys[i], keys[j]);

      if (i < poses.size()) {
        Pose2 R(result_.at<Pose2>(keys[i]).r(), Point2());
        Sij = R.matrix() * Sij;
      }

      if (j < poses.size()) {
        Pose2 R(result_.at<Pose2>(keys[j]).r(), Point2());
        Sij = Sij * R.matrix().transpose();
      }

      S.block(p, q, Sij.rows(), Sij.cols()) = Sij;
      q += Sij.cols();
      if (j == keys.size() - 1) {
        p += Sij.rows();
        q = p;
      }
    }
  }
  S.triangularView<Eigen::Lower>() = S.transpose();
  return S;
#endif
}

std::shared_ptr<const ISAM2> SLAM2D::getISAM2() const {
  return std::const_pointer_cast<const ISAM2>(isam_);
}

void SLAM2D::optimize(bool update_covariance) {
  if (optimized_)
    return;

  if (graph_.size() == 0)
    return;

  isam_->update(graph_, initial_estimate_);
  result_ = isam_->calculateEstimate();

  for (int i = 0; i < step_; ++i) {
    Symbol x = getVehicleSymbol(i);
    Pose2 pose = result_.at<Pose2>(x);
    if (update_covariance) {
#ifdef USE_FAST_MARGINAL
      Eigen::Matrix3d covariance = jointMarginalCovarianceLocal({i}, {});
#else
      Eigen::Matrix3d covariance = isam_->marginalCovariance(x);
#endif
      Pose2 R(pose.r(), Point2());
      covariance = R.matrix() * covariance * R.matrix().transpose();

      VehicleBeliefState state(pose, inverse(covariance));
      if (i < map_.getTrajectorySize()) {
        state.core_vehicle = map_.getVehicle(i).core_vehicle;
        map_.updateVehicle(i, state);
      } else {
        state.core_vehicle = (i == step_ - 1);
        map_.addVehicle(state);
      }
    } else {
      if (i < map_.getTrajectorySize())
        map_.updateVehicle(i, pose);
      else
        map_.addVehicle(pose);
    }
  }
  for (auto it = map_.beginLandmark(); it != map_.endLandmark(); ++it) {
    Symbol l = getLandmarkSymbol(it->first);
    it->second.point = result_.at<Point2>(l);
    if (update_covariance)
#ifdef USE_FAST_MARGINAL
      it->second.information = jointMarginalCovarianceLocal({}, {symbolIndex(l)}).inverse();
#else
      it->second.information = isam_->marginalCovariance(l).inverse();
#endif
  }

  graph_.resize(0);
  initial_estimate_.clear();
  optimized_ = true;
  marginals_update_ = false;
}

std::pair<double, std::shared_ptr<Map>> SLAM2D::sample() const {
  FastVector<ISAM2Clique::shared_ptr> roots = isam_->roots();

  double lp = 0.0;
  VectorValues result(isam_->getDelta());
  for (const ISAM2Clique::shared_ptr &root : roots) {
    lp += optimizeInPlacePerturbation(root, result);
  }
  Values values = isam_->getLinearizationPoint().retract(result);

  std::shared_ptr<Map> sampled_map(new Map(map_.getParameter()));
  for (int i = 0; i < step_; ++i)
    sampled_map->addVehicle(values.at<Pose2>(getVehicleSymbol(i)));

  for (auto it = map_.cbeginLandmark(); it != map_.cendLandmark(); ++it)
    sampled_map->addLandmark(it->first, values.at<Point2>(getLandmarkSymbol(it->first)));

  return std::make_pair(lp, sampled_map);
}

double SLAM2D::optimizeInPlacePerturbation(const ISAM2Clique::shared_ptr &clique, VectorValues &result) const {
  GaussianConditional::shared_ptr conditional = clique->conditional();
  const Vector xS = result.vector(FastVector<Key>(conditional->beginParents(), conditional->endParents()));

  Vector rhs = conditional->get_d() - conditional->get_S() * xS;

  double lp = 0.0;
  for (int i = 0; i < rhs.rows(); ++i) {
    double n = rng_.normal01();
    rhs(i) += n;
    lp += -0.5 * n * n;
  }

  const Vector solution = conditional->get_R().triangularView<Eigen::Upper>().solve(rhs);

  if (solution.hasNaN()) {
    throw IndeterminantLinearSystemException(conditional->keys().front());
  }

  DenseIndex vectorPosition = 0;
  for (GaussianConditional::const_iterator frontal = conditional->beginFrontals();
       frontal != conditional->endFrontals(); ++frontal) {
    result.at(*frontal) = solution.segment(vectorPosition, conditional->getDim(frontal));
    vectorPosition += conditional->getDim(frontal);
  }

  for(const ISAM2Clique::shared_ptr &child: clique->children)
    lp += optimizeInPlacePerturbation(child, result);

  return lp;
}

}
