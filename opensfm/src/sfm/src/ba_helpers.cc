#include <bundle/bundle_adjuster.h>
#include <foundation/types.h>
#include <geometry/triangulation.h>
#include <map/ground_control_points.h>
#include <map/map.h>
#include <sfm/ba_helpers.h>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

#include "geo/geo.h"
#include "map/defines.h"

namespace sfm {
std::pair<std::unordered_set<map::ShotId>, std::unordered_set<map::ShotId>>
BAHelpers::ShotNeighborhoodIds(map::Map& map,
                               const map::ShotId& central_shot_id,
                               size_t radius, size_t min_common_points,
                               size_t max_interior_size) {
  auto res = ShotNeighborhood(map, central_shot_id, radius, min_common_points,
                              max_interior_size);
  std::unordered_set<map::ShotId> interior;
  for (map::Shot* shot : res.first) {
    interior.insert(shot->GetId());
  }
  std::unordered_set<map::ShotId> boundary;
  for (map::Shot* shot : res.second) {
    boundary.insert(shot->GetId());
  }
  return std::make_pair(interior, boundary);
}

/**Reconstructed shots near a given shot.

Returns:
    a tuple with interior and boundary:
    - interior: the list of shots at distance smaller than radius
    - boundary: shots sharing at least on point with the interior

Central shot is at distance 0.  Shots at distance n + 1 share at least
min_common_points points with shots at distance n.
*/
std::pair<std::unordered_set<map::Shot*>, std::unordered_set<map::Shot*>>
BAHelpers::ShotNeighborhood(map::Map& map, const map::ShotId& central_shot_id,
                            size_t radius, size_t min_common_points,
                            size_t max_interior_size) {
  constexpr size_t MaxBoundarySize{1000000};
  std::unordered_set<map::Shot*> interior;
  auto& central_shot = map.GetShot(central_shot_id);
  const auto instance_shot =
      map.GetRigInstance(central_shot.GetRigInstanceId()).GetShotIDs();
  for (const auto& s : instance_shot) {
    interior.insert(&map.GetShot(s));
  }
  interior.insert(&central_shot);
  for (size_t distance = 1;
       distance < radius && interior.size() < max_interior_size; ++distance) {
    const auto remaining = max_interior_size - interior.size();
    const auto neighbors =
        DirectShotNeighbors(map, interior, min_common_points, remaining);
    interior.insert(neighbors.begin(), neighbors.end());
  }

  const auto boundary = DirectShotNeighbors(map, interior, 1, MaxBoundarySize);
  return std::make_pair(interior, boundary);
}

std::unordered_set<map::Shot*> BAHelpers::DirectShotNeighbors(
    map::Map& map, const std::unordered_set<map::Shot*>& shot_ids,
    const size_t min_common_points, const size_t max_neighbors) {
  std::unordered_set<map::Landmark*> points;
  for (auto* shot : shot_ids) {
    for (const auto& lm_obs : shot->GetLandmarkObservations()) {
      points.insert(lm_obs.first);
    }
  }

  std::unordered_map<map::Shot*, size_t> common_points;
  for (auto* pt : points) {
    for (const auto& neighbor_p : pt->GetObservations()) {
      auto* shot = neighbor_p.first;
      if (shot_ids.find(shot) == shot_ids.end()) {
        ++common_points[shot];
      }
    }
  }

  std::vector<std::pair<map::Shot*, size_t>> pairs(common_points.begin(),
                                                   common_points.end());
  std::sort(pairs.begin(), pairs.end(),
            [](const std::pair<map::Shot*, size_t>& val1,
               const std::pair<map::Shot*, size_t>& val2) {
              return val1.second > val2.second;
            });

  const size_t max_n = std::min(max_neighbors, pairs.size());
  std::unordered_set<map::Shot*> neighbors;
  size_t idx = 0;
  for (auto& p : pairs) {
    if (p.second >= min_common_points && idx < max_n) {
      const auto instance_shots =
          map.GetRigInstance(p.first->GetRigInstanceId()).GetShotIDs();
      for (const auto& s : instance_shots) {
        neighbors.insert(&map.GetShot(s));
      }
    } else {
      break;
    }
    ++idx;
  }
  return neighbors;
}

py::tuple BAHelpers::BundleLocal(
    map::Map& map,
    const std::unordered_map<map::CameraId, geometry::Camera>& camera_priors,
    const std::unordered_map<map::RigCameraId, map::RigCamera>&
        rig_camera_priors,
    const AlignedVector<map::GroundControlPoint>& gcp,
    const map::ShotId& central_shot_id, const py::dict& config) {
  py::dict report;
  const auto start = std::chrono::high_resolution_clock::now();
  auto neighborhood = ShotNeighborhood(
      map, central_shot_id, config["local_bundle_radius"].cast<size_t>(),
      config["local_bundle_min_common_points"].cast<size_t>(),
      config["local_bundle_max_shots"].cast<size_t>());
  auto& interior = neighborhood.first;
  auto& boundary = neighborhood.second;

  // set up BA
  auto ba = bundle::BundleAdjuster();
  ba.SetUseAnalyticDerivatives(
      config["bundle_analytic_derivatives"].cast<bool>());

  for (const auto& cam_pair : map.GetCameras()) {
    const auto& cam = cam_pair.second;
    const auto& cam_prior = camera_priors.at(cam.id);
    constexpr bool fix_cameras{true};
    ba.AddCamera(cam.id, cam, cam_prior, fix_cameras);
  }
  // combine the sets
  std::unordered_set<map::Shot*> int_and_bound(interior.cbegin(),
                                               interior.cend());
  int_and_bound.insert(boundary.cbegin(), boundary.cend());
  std::unordered_set<map::Landmark*> points;
  py::list pt_ids;

  constexpr bool point_constant{false};
  constexpr bool rig_camera_constant{true};

  // gather required rig data to setup
  std::unordered_set<map::RigCameraId> rig_cameras_ids;
  std::unordered_set<map::RigInstanceId> rig_instances_ids;
  for (auto* shot : int_and_bound) {
    rig_cameras_ids.insert(shot->GetRigCameraId());
    rig_instances_ids.insert(shot->GetRigInstanceId());
  }

  // rig cameras are going to be fixed
  for (const auto& rig_camera_id : rig_cameras_ids) {
    const auto& rig_camera = map.GetRigCamera(rig_camera_id);
    ba.AddRigCamera(rig_camera_id, rig_camera.pose,
                    rig_camera_priors.at(rig_camera_id).pose,
                    rig_camera_constant);
  }

  // add rig instances shots
  const std::string gps_scale_group = "dummy";  // unused for now
  for (const auto& rig_instance_id : rig_instances_ids) {
    auto& instance = map.GetRigInstance(rig_instance_id);
    std::unordered_map<std::string, std::string> shot_cameras, shot_rig_cameras;

    // we're going to assign GPS constraint to the instance itself
    // by averaging its shot's GPS values (and std dev.)
    Vec3d average_position = Vec3d::Zero();
    double average_std = 0.;
    int gps_count = 0;

    // if any instance's shot is in boundary
    // then the entire instance will be fixed
    bool fix_instance = false;
    for (const auto& shot_n_rig_camera : instance.GetRigCameras()) {
      const auto shot_id = shot_n_rig_camera.first;
      auto& shot = map.GetShot(shot_id);
      shot_cameras[shot_id] = shot.GetCamera()->id;
      shot_rig_cameras[shot_id] = shot_n_rig_camera.second->id;

      const auto is_boundary = boundary.find(&shot) != boundary.end();
      const auto is_interior = !is_boundary;

      if (is_interior) {
        const auto& measurements = shot.GetShotMeasurements();
        if (config["bundle_use_gps"].cast<bool>() &&
            measurements.gps_position_.HasValue()) {
          average_position += measurements.gps_position_.Value();
          average_std += measurements.gps_accuracy_.Value();
          ++gps_count;
        }
      } else {
        fix_instance = true;
      }
    }

    ba.AddRigInstance(rig_instance_id, instance.GetPose(), shot_cameras,
                      shot_rig_cameras, fix_instance);

    // only add averaged rig position constraints to moving instances
    if (!fix_instance && gps_count > 0) {
      average_position /= gps_count;
      average_std /= gps_count;
      ba.AddRigInstancePositionPrior(rig_instance_id, average_position,
                                     Vec3d::Constant(average_std),
                                     gps_scale_group);
    }
  }

  size_t added_landmarks = 0;
  size_t added_reprojections = 0;
  for (auto* shot : interior) {
    // Add all points of the shots that are in the interior
    for (const auto& lm_obs : shot->GetLandmarkObservations()) {
      auto* lm = lm_obs.first;
      if (points.count(lm) == 0) {
        points.insert(lm);
        pt_ids.append(lm->id_);
        ba.AddPoint(lm->id_, lm->GetGlobalPos(), point_constant);
        ++added_landmarks;
      }
      const auto& obs = lm_obs.second;
      ba.AddPointProjectionObservation(shot->id_, lm_obs.first->id_, obs.point,
                                       obs.scale, obs.depth_prior);
      ++added_reprojections;
    }
  }
  for (auto* shot : boundary) {
    for (const auto& lm_obs : shot->GetLandmarkObservations()) {
      auto* lm = lm_obs.first;
      if (points.count(lm) > 0) {
        const auto& obs = lm_obs.second;
        ba.AddPointProjectionObservation(shot->id_, lm_obs.first->id_,
                                         obs.point, obs.scale, obs.depth_prior);
        ++added_reprojections;
      }
    }
  }

  if (config["bundle_use_gcp"].cast<bool>() && !gcp.empty()) {
    AddGCPToBundle(ba, map, gcp, config);
  }

  ba.SetPointProjectionLossFunction(
      config["loss_function"].cast<std::string>(),
      config["loss_function_threshold"].cast<double>());
  ba.SetInternalParametersPriorSD(
      config["exif_focal_sd"].cast<double>(),
      config["principal_point_sd"].cast<double>(),
      config["radial_distortion_k1_sd"].cast<double>(),
      config["radial_distortion_k2_sd"].cast<double>(),
      config["tangential_distortion_p1_sd"].cast<double>(),
      config["tangential_distortion_p2_sd"].cast<double>(),
      config["radial_distortion_k3_sd"].cast<double>(),
      config["radial_distortion_k4_sd"].cast<double>());
  ba.SetRigParametersPriorSD(config["rig_translation_sd"].cast<double>(),
                             config["rig_rotation_sd"].cast<double>());

  ba.SetNumThreads(config["processes"].cast<int>());
  ba.SetMaxNumIterations(10);
  ba.SetLinearSolverType("DENSE_SCHUR");
  const auto timer_setup = std::chrono::high_resolution_clock::now();

  {
    py::gil_scoped_release release;
    ba.Run();
  }

  const auto timer_run = std::chrono::high_resolution_clock::now();
  for (const auto& rig_instance_id : rig_instances_ids) {
    auto& instance = map.GetRigInstance(rig_instance_id);
    auto i = ba.GetRigInstance(rig_instance_id);
    instance.SetPose(i.GetValue());
  }

  for (auto* point : points) {
    const auto& pt = ba.GetPoint(point->id_);
    point->SetGlobalPos(pt.GetValue());
    point->SetReprojectionErrors(pt.reprojection_errors);
  }
  const auto timer_teardown = std::chrono::high_resolution_clock::now();
  report["brief_report"] = ba.BriefReport();
  report["wall_times"] = py::dict();
  report["wall_times"]["setup"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_setup - start)
          .count() /
      1000000.0;
  report["wall_times"]["run"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_run -
                                                            timer_setup)
          .count() /
      1000000.0;
  report["wall_times"]["teardown"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_teardown -
                                                            timer_run)
          .count() /
      1000000.0;
  report["num_images"] = interior.size();
  report["num_interior_images"] = interior.size();
  report["num_boundary_images"] = boundary.size();
  report["num_other_images"] =
      map.NumberOfShots() - interior.size() - boundary.size();
  report["num_points"] = added_landmarks;
  report["num_reprojections"] = added_reprojections;
  return py::make_tuple(pt_ids, report);
}

bool BAHelpers::TriangulateGCP(
    const map::GroundControlPoint& point,
    const std::unordered_map<map::ShotId, map::Shot>& shots,
    Vec3d& coordinates) {
  constexpr auto reproj_threshold{1.0};
  constexpr auto min_ray_angle = 0.1 * M_PI / 180.0;
  constexpr auto min_depth = 1e-3;  // Assume GCPs 1mm+ away from the camera
  MatX3d os, bs;
  size_t added = 0;
  coordinates = Vec3d::Zero();
  bs.conservativeResize(point.observations_.size(), Eigen::NoChange);
  os.conservativeResize(point.observations_.size(), Eigen::NoChange);
  for (const auto& obs : point.observations_) {
    const auto shot_it = shots.find(obs.shot_id_);
    if (shot_it != shots.end()) {
      const auto& shot = (shot_it->second);
      const Vec3d bearing = shot.GetCamera()->Bearing(obs.projection_);
      const auto& shot_pose = shot.GetPose();
      bs.row(added) = shot_pose->RotationCameraToWorld() * bearing;
      os.row(added) = shot_pose->GetOrigin();
      ++added;
    }
  }
  bs.conservativeResize(added, Eigen::NoChange);
  os.conservativeResize(added, Eigen::NoChange);
  if (added >= 2) {
    const std::vector<double> thresholds(added, reproj_threshold);
    const auto& res = geometry::TriangulateBearingsMidpoint(
        os, bs, thresholds, min_ray_angle, min_depth);
    coordinates = res.second;
    return res.first;
  }
  return false;
}

// Add Ground Control Points constraints to the bundle problem
size_t BAHelpers::AddGCPToBundle(
    bundle::BundleAdjuster& ba, const map::Map& map,
    const AlignedVector<map::GroundControlPoint>& gcp, const py::dict& config) {
  const auto& reference = map.GetTopocentricConverter();
  const auto& shots = map.GetShots();

  const auto dominant_terms = ba.GetRigInstances().size() +
                              ba.GetProjectionsCount() +
                              ba.GetRelativeMotionsCount();

  size_t total_terms = 0;
  for (const auto& point : gcp) {
    Vec3d coordinates;
    if (TriangulateGCP(point, shots, coordinates) || !point.lla_.empty()) {
      ++total_terms;
    }
    for (const auto& obs : point.observations_) {
      total_terms += (shots.count(obs.shot_id_) > 0);
    }
  }

  double global_weight = config["gcp_global_weight"].cast<double>() *
                         dominant_terms / std::max<size_t>(1, total_terms);

  size_t added_gcp_observations = 0;
  for (const auto& point : gcp) {
    const auto point_id = "gcp-" + point.id_;
    Vec3d coordinates;
    if (!TriangulateGCP(point, shots, coordinates)) {
      if (!point.lla_.empty()) {
        coordinates = reference.ToTopocentric(point.GetLlaVec3d());
      } else {
        continue;
      }
    }
    constexpr auto point_constant{false};
    ba.AddPoint(point_id, coordinates, point_constant);
    if (!point.lla_.empty()) {
      const auto point_std = Vec3d(config["gcp_horizontal_sd"].cast<double>(),
                                   config["gcp_horizontal_sd"].cast<double>(),
                                   config["gcp_vertical_sd"].cast<double>());
      ba.AddPointPrior(point_id, reference.ToTopocentric(point.GetLlaVec3d()),
                       point_std / global_weight, point.has_altitude_);
    }

    // Now iterate through the observations
    for (const auto& obs : point.observations_) {
      const auto& shot_id = obs.shot_id_;
      if (shots.count(shot_id) > 0) {
        constexpr double scale{0.001};
        ba.AddPointProjectionObservation(shot_id, point_id, obs.projection_,
                                         scale / global_weight);
        ++added_gcp_observations;
      }
    }
  }
  return added_gcp_observations;
}

py::dict BAHelpers::BundleShotPoses(
    map::Map& map, const std::unordered_set<map::ShotId>& shot_ids,
    const std::unordered_map<map::CameraId, geometry::Camera>& camera_priors,
    const std::unordered_map<map::RigCameraId, map::RigCamera>&
        rig_camera_priors,
    const py::dict& config) {
  py::dict report;

  constexpr auto fix_cameras = true;
  constexpr auto fix_points = true;
  constexpr auto fix_rig_camera = true;

  auto ba = bundle::BundleAdjuster();
  ba.SetUseAnalyticDerivatives(
      config["bundle_analytic_derivatives"].cast<bool>());
  const auto start = std::chrono::high_resolution_clock::now();

  // gather required rig data to setup
  std::unordered_set<map::RigInstanceId> rig_instances_ids;
  for (const auto& shot_id : shot_ids) {
    const auto& shot = map.GetShot(shot_id);
    rig_instances_ids.insert(shot.GetRigInstanceId());
  }
  std::unordered_set<map::RigCameraId> rig_cameras_ids;
  std::unordered_set<map::CameraId> cameras_ids;
  for (const auto& rig_instance_id : rig_instances_ids) {
    auto& instance = map.GetRigInstance(rig_instance_id);
    for (const auto& shot_n_rig_camera : instance.GetRigCameras()) {
      const auto rig_camera_id = shot_n_rig_camera.second->id;
      rig_cameras_ids.insert(rig_camera_id);

      const auto shot_id = shot_n_rig_camera.first;
      const auto camera_id = map.GetShot(shot_id).GetCamera()->id;
      cameras_ids.insert(camera_id);
    }
  }

  // rig cameras are going to be fixed
  for (const auto& rig_camera_id : rig_cameras_ids) {
    const auto& rig_camera = map.GetRigCamera(rig_camera_id);
    ba.AddRigCamera(rig_camera_id, rig_camera.pose,
                    rig_camera_priors.at(rig_camera_id).pose, fix_rig_camera);
  }

  for (const auto& camera_id : cameras_ids) {
    const auto& cam = map.GetCamera(camera_id);
    const auto& cam_prior = camera_priors.at(camera_id);
    ba.AddCamera(camera_id, cam, cam_prior, fix_cameras);
  }

  std::unordered_set<map::Landmark*> landmarks;
  for (const auto& shot_id : shot_ids) {
    const auto& shot = map.GetShot(shot_id);
    for (const auto& lm_obs : shot.GetLandmarkObservations()) {
      landmarks.insert(lm_obs.first);
    }
  }
  for (const auto& landmark : landmarks) {
    ba.AddPoint(landmark->id_, landmark->GetGlobalPos(), fix_points);
  }

  // add rig instances shots
  const std::string gps_scale_group = "dummy";  // unused for now
  for (const auto& rig_instance_id : rig_instances_ids) {
    auto& instance = map.GetRigInstance(rig_instance_id);
    std::unordered_map<std::string, std::string> shot_cameras, shot_rig_cameras;

    // we're going to assign GPS constraint to the instance itself
    // by averaging its shot's GPS values (and std dev.)
    Vec3d average_position = Vec3d::Zero();
    double average_std = 0.;
    int gps_count = 0;

    // if any instance's shot is in boundary
    // then the entire instance will be fixed
    bool fix_instance = false;

    for (const auto& shot_n_rig_camera : instance.GetRigCameras()) {
      const auto shot_id = shot_n_rig_camera.first;
      auto& shot = map.GetShot(shot_id);
      shot_cameras[shot_id] = shot.GetCamera()->id;
      shot_rig_cameras[shot_id] = shot_n_rig_camera.second->id;

      const auto is_fixed = shot_ids.find(shot_id) != shot_ids.end();
      if (!is_fixed) {
        if (config["bundle_use_gps"].cast<bool>()) {
          const auto pos = shot.GetShotMeasurements().gps_position_;
          const auto acc = shot.GetShotMeasurements().gps_accuracy_;
          if (pos.HasValue() && acc.HasValue()) {
            average_position += pos.Value();
            average_std += acc.Value();
            ++gps_count;
          }
        }
      } else {
        fix_instance = true;
      }

      ba.AddRigInstance(rig_instance_id, instance.GetPose(), shot_cameras,
                        shot_rig_cameras, fix_instance);

      // only add averaged rig position constraints to moving instances
      if (!fix_instance && gps_count > 0) {
        average_position /= gps_count;
        average_std /= gps_count;
        ba.AddRigInstancePositionPrior(rig_instance_id, average_position,
                                       Vec3d::Constant(average_std),
                                       gps_scale_group);
      }
    }
  }

  // add observations
  for (const auto& shot_id : shot_ids) {
    const auto& shot = map.GetShot(shot_id);
    for (const auto& lm_obs : shot.GetLandmarkObservations()) {
      const auto& obs = lm_obs.second;
      ba.AddPointProjectionObservation(shot.id_, lm_obs.first->id_, obs.point,
                                       obs.scale, obs.depth_prior);
    }
  }

  ba.SetPointProjectionLossFunction(
      config["loss_function"].cast<std::string>(),
      config["loss_function_threshold"].cast<double>());
  ba.SetInternalParametersPriorSD(
      config["exif_focal_sd"].cast<double>(),
      config["principal_point_sd"].cast<double>(),
      config["radial_distortion_k1_sd"].cast<double>(),
      config["radial_distortion_k2_sd"].cast<double>(),
      config["tangential_distortion_p1_sd"].cast<double>(),
      config["tangential_distortion_p2_sd"].cast<double>(),
      config["radial_distortion_k3_sd"].cast<double>(),
      config["radial_distortion_k4_sd"].cast<double>());
  ba.SetRigParametersPriorSD(config["rig_translation_sd"].cast<double>(),
                             config["rig_rotation_sd"].cast<double>());

  ba.SetNumThreads(config["processes"].cast<int>());
  ba.SetMaxNumIterations(10);
  ba.SetLinearSolverType("DENSE_QR");
  const auto timer_setup = std::chrono::high_resolution_clock::now();

  {
    py::gil_scoped_release release;
    ba.Run();
  }

  const auto timer_run = std::chrono::high_resolution_clock::now();

  for (const auto& rig_instance_id : rig_instances_ids) {
    auto& instance = map.GetRigInstance(rig_instance_id);
    auto i = ba.GetRigInstance(rig_instance_id);
    instance.SetPose(i.GetValue());
  }

  const auto timer_teardown = std::chrono::high_resolution_clock::now();
  report["brief_report"] = ba.BriefReport();
  report["wall_times"] = py::dict();
  report["wall_times"]["setup"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_setup - start)
          .count() /
      1000000.0;
  report["wall_times"]["run"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_run -
                                                            timer_setup)
          .count() /
      1000000.0;
  report["wall_times"]["teardown"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_teardown -
                                                            timer_run)
          .count() /
      1000000.0;
  return report;
}

py::dict BAHelpers::Bundle(
    map::Map& map,
    const std::unordered_map<map::CameraId, geometry::Camera>& camera_priors,
    const std::unordered_map<map::RigCameraId, map::RigCamera>&
        rig_camera_priors,
    const AlignedVector<map::GroundControlPoint>& gcp, const py::dict& config) {
  py::dict report;

  auto ba = bundle::BundleAdjuster();
  const bool fix_cameras = !config["optimize_camera_parameters"].cast<bool>();
  ba.SetUseAnalyticDerivatives(
      config["bundle_analytic_derivatives"].cast<bool>());
  const auto start = std::chrono::high_resolution_clock::now();

  const auto& all_cameras = map.GetCameras();
  for (const auto& cam_pair : all_cameras) {
    const auto& cam = cam_pair.second;
    const auto& cam_prior = camera_priors.at(cam.id);
    ba.AddCamera(cam.id, cam, cam_prior, fix_cameras);
  }

  for (const auto& pt_pair : map.GetLandmarks()) {
    const auto& pt = pt_pair.second;
    ba.AddPoint(pt.id_, pt.GetGlobalPos(), false);
  }

  auto align_method = config["align_method"].cast<std::string>();
  if (align_method.compare("auto") == 0) {
    align_method = DetectAlignmentConstraints(map, config, gcp);
  }
  bool do_add_align_vector = false;
  Vec3d up_vector = Vec3d::Zero();
  if (align_method.compare("orientation_prior") == 0) {
    const std::string align_orientation_prior =
        config["align_orientation_prior"].cast<std::string>();
    if (align_orientation_prior.compare("vertical") == 0) {
      do_add_align_vector = true;
      up_vector = Vec3d(0, 0, -1);
    } else if (align_orientation_prior.compare("horizontal") == 0) {
      do_add_align_vector = true;
      up_vector = Vec3d(0, -1, 0);
    }
  }

  // setup rig cameras
  constexpr size_t kMinRigInstanceForAdjust{10};
  const size_t shots_per_rig_cameras =
      map.GetRigCameras().size() > 0
          ? static_cast<size_t>(map.GetShots().size() /
                                map.GetRigCameras().size())
          : 1;
  const auto lock_rig_camera =
      shots_per_rig_cameras <= kMinRigInstanceForAdjust;
  for (const auto& camera_pair : map.GetRigCameras()) {
    // could be set to false (not locked) the day we expose leverarm adjustment
    const bool is_leverarm =
        all_cameras.find(camera_pair.first) != all_cameras.end();
    ba.AddRigCamera(camera_pair.first, camera_pair.second.pose,
                    rig_camera_priors.at(camera_pair.first).pose,
                    is_leverarm | lock_rig_camera);
  }

  // setup rig instances
  const std::string gps_scale_group = "dummy";  // unused for now
  for (auto instance_pair : map.GetRigInstances()) {
    auto& instance = instance_pair.second;

    Vec3d average_position = Vec3d::Zero();
    double average_std = 0.;
    int gps_count = 0;

    // average GPS and assign GPS constraint to the instance
    std::unordered_map<std::string, std::string> shot_cameras, shot_rig_cameras;
    for (const auto& shot_n_rig_camera : instance.GetRigCameras()) {
      const auto shot_id = shot_n_rig_camera.first;
      const auto& shot = map.GetShot(shot_id);
      shot_cameras[shot_id] = shot.GetCamera()->id;
      shot_rig_cameras[shot_id] = shot_n_rig_camera.second->id;

      if (config["bundle_use_gps"].cast<bool>()) {
        const auto pos = shot.GetShotMeasurements().gps_position_;
        const auto acc = shot.GetShotMeasurements().gps_accuracy_;
        if (pos.HasValue() && acc.HasValue()) {
          if (acc.Value() <= 0) {
            throw std::runtime_error(
                "Shot " + shot.GetId() +
                " has an accuracy <= 0: " + std::to_string(acc.Value()) +
                ". Try modifying "
                "your input parser to filter such values.");
          }
          average_position += pos.Value();
          average_std += acc.Value();
          ++gps_count;
        }
      }
    }

    ba.AddRigInstance(instance_pair.first, instance.GetPose(), shot_cameras,
                      shot_rig_cameras, false);

    if (config["bundle_use_gps"].cast<bool>() && gps_count > 0) {
      average_position /= gps_count;
      average_std /= gps_count;
      ba.AddRigInstancePositionPrior(instance_pair.first, average_position,
                                     Vec3d::Constant(average_std),
                                     gps_scale_group);
    }
  }

  size_t added_reprojections = 0;
  for (const auto& shot_pair : map.GetShots()) {
    const auto& shot = shot_pair.second;

    // that one doesn't have it's rig counterpart
    if (do_add_align_vector) {
      constexpr double std_dev = 1e-3;
      ba.AddAbsoluteUpVector(shot.id_, up_vector, std_dev);
    }

    // setup observations for any shot type
    for (const auto& lm_obs : shot.GetLandmarkObservations()) {
      const auto& obs = lm_obs.second;
      ba.AddPointProjectionObservation(shot.id_, lm_obs.first->id_, obs.point,
                                       obs.scale, obs.depth_prior);
      ++added_reprojections;
    }
  }

  if (config["bundle_use_gcp"].cast<bool>() && !gcp.empty()) {
    AddGCPToBundle(ba, map, gcp, config);
  }

  if (config["bundle_compensate_gps_bias"].cast<bool>()) {
    const auto& biases = map.GetBiases();
    for (const auto& camera : map.GetCameras()) {
      ba.SetCameraBias(camera.first, biases.at(camera.first));
    }
  }

  ba.SetPointProjectionLossFunction(
      config["loss_function"].cast<std::string>(),
      config["loss_function_threshold"].cast<double>());
  ba.SetInternalParametersPriorSD(
      config["exif_focal_sd"].cast<double>(),
      config["principal_point_sd"].cast<double>(),
      config["radial_distortion_k1_sd"].cast<double>(),
      config["radial_distortion_k2_sd"].cast<double>(),
      config["tangential_distortion_p1_sd"].cast<double>(),
      config["tangential_distortion_p2_sd"].cast<double>(),
      config["radial_distortion_k3_sd"].cast<double>(),
      config["radial_distortion_k4_sd"].cast<double>());
  ba.SetRigParametersPriorSD(config["rig_translation_sd"].cast<double>(),
                             config["rig_rotation_sd"].cast<double>());

  ba.SetNumThreads(config["processes"].cast<int>());
  ba.SetMaxNumIterations(config["bundle_max_iterations"].cast<int>());
  ba.SetLinearSolverType("SPARSE_SCHUR");
  const auto timer_setup = std::chrono::high_resolution_clock::now();

  {
    py::gil_scoped_release release;
    ba.Run();
  }

  const auto timer_run = std::chrono::high_resolution_clock::now();

  BundleToMap(ba, map, !fix_cameras);

  const auto timer_teardown = std::chrono::high_resolution_clock::now();
  report["brief_report"] = ba.BriefReport();
  report["wall_times"] = py::dict();
  report["wall_times"]["setup"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_setup - start)
          .count() /
      1000000.0;
  report["wall_times"]["run"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_run -
                                                            timer_setup)
          .count() /
      1000000.0;
  report["wall_times"]["teardown"] =
      std::chrono::duration_cast<std::chrono::microseconds>(timer_teardown -
                                                            timer_run)
          .count() /
      1000000.0;
  report["num_images"] = map.GetShots().size();
  report["num_points"] = map.GetLandmarks().size();
  report["num_reprojections"] = added_reprojections;
  return report;
}

void BAHelpers::BundleToMap(const bundle::BundleAdjuster& bundle_adjuster,
                            map::Map& output_map, bool update_cameras) {
  // update cameras
  if (update_cameras) {
    for (auto& cam : output_map.GetCameras()) {
      const auto& ba_cam = bundle_adjuster.GetCamera(cam.first);
      for (const auto& p : ba_cam.GetParametersMap()) {
        cam.second.SetParameterValue(p.first, p.second);
      }
    }
  }

  // Update bias
  for (auto& bias : output_map.GetBiases()) {
    const auto& new_bias = bundle_adjuster.GetBias(bias.first);
    if (!new_bias.IsValid()) {
      throw std::runtime_error("Bias " + bias.first +
                               " has either NaN or INF values.");
    }
    bias.second = new_bias;
  }

  // Update rig instances
  for (auto& instance : output_map.GetRigInstances()) {
    const auto new_instance =
        bundle_adjuster.GetRigInstance(instance.first).GetValue();
    if (!new_instance.IsValid()) {
      throw std::runtime_error("Rig Instance " + instance.first +
                               " has either NaN or INF values.");
    }
    instance.second.SetPose(new_instance);
  }

  // Update rig cameras
  for (auto& rig_camera : output_map.GetRigCameras()) {
    const auto new_rig_camera =
        bundle_adjuster.GetRigCamera(rig_camera.first).GetValue();
    if (!new_rig_camera.IsValid()) {
      throw std::runtime_error("Rig Camera " + rig_camera.first +
                               " has either NaN or INF values.");
    }
    rig_camera.second.pose = new_rig_camera;
  }

  // Update points
  for (auto& point : output_map.GetLandmarks()) {
    const auto& pt = bundle_adjuster.GetPoint(point.first);
    if (!pt.GetValue().allFinite()) {
      throw std::runtime_error("Point " + point.first +
                               " has either NaN or INF values.");
    }
    point.second.SetGlobalPos(pt.GetValue());
    point.second.SetReprojectionErrors(pt.reprojection_errors);
  }
}

void BAHelpers::AlignmentConstraints(
    const map::Map& map, const py::dict& config,
    const AlignedVector<map::GroundControlPoint>& gcp, MatX3d& Xp, MatX3d& X) {
  size_t reserve_size = 0;
  const auto& shots = map.GetShots();
  if (!gcp.empty() && config["bundle_use_gcp"].cast<bool>()) {
    reserve_size += gcp.size();
  }
  if (config["bundle_use_gps"].cast<bool>()) {
    for (const auto& shot_p : shots) {
      const auto& shot = shot_p.second;
      if (shot.GetShotMeasurements().gps_position_.HasValue()) {
        reserve_size += 1;
      }
    }
  }
  Xp.conservativeResize(reserve_size, Eigen::NoChange);
  X.conservativeResize(reserve_size, Eigen::NoChange);
  const auto& topocentricConverter = map.GetTopocentricConverter();
  size_t idx = 0;
  // Triangulated vs measured points
  if (!gcp.empty() && config["bundle_use_gcp"].cast<bool>()) {
    for (const auto& point : gcp) {
      if (point.lla_.empty()) {
        continue;
      }
      Vec3d coordinates;
      if (TriangulateGCP(point, shots, coordinates)) {
        Xp.row(idx) = topocentricConverter.ToTopocentric(point.GetLlaVec3d());
        X.row(idx) = coordinates;
        ++idx;
      }
    }
  }
  if (config["bundle_use_gps"].cast<bool>()) {
    for (const auto& shot_p : shots) {
      const auto& shot = shot_p.second;
      const auto pos = shot.GetShotMeasurements().gps_position_;
      if (pos.HasValue()) {
        Xp.row(idx) = pos.Value();
        X.row(idx) = shot.GetPose()->GetOrigin();
        ++idx;
      }
    }
  }
}

std::string BAHelpers::DetectAlignmentConstraints(
    const map::Map& map, const py::dict& config,
    const AlignedVector<map::GroundControlPoint>& gcp) {
  MatX3d X, Xp;
  AlignmentConstraints(map, config, gcp, Xp, X);
  if (X.rows() < 3) {
    return "orientation_prior";
  }
  const Vec3d X_mean = X.colwise().mean();
  const MatX3d X_zero = X.rowwise() - X_mean.transpose();
  const Mat3d input = X_zero.transpose() * X_zero;
  Eigen::SelfAdjointEigenSolver<MatXd> ses(input, Eigen::EigenvaluesOnly);
  const Vec3d evals = ses.eigenvalues();
  const auto ratio_1st_2nd = std::abs(evals[2] / evals[1]);
  constexpr double epsilon_abs = 1e-10;
  constexpr double epsilon_ratio = 5e3;
  int cond1 = 0;
  for (int i = 0; i < 3; ++i) {
    cond1 += (evals[i] < epsilon_abs) ? 1 : 0;
  }
  const bool is_line = cond1 > 1 || ratio_1st_2nd > epsilon_ratio;
  if (is_line) {
    return "orientation_prior";
  }

  return "naive";
}
}  // namespace sfm
