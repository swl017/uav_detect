#include "main.h"

#include <nodelet/nodelet.h>

#include <pcl/ModelCoefficients.h>
#include <pcl/common/transforms.h>
#include <pcl/conversions.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/registration/transforms.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/segmentation/conditional_euclidean_clustering.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl_conversions/pcl_conversions.h>

#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/crop_box.h>

#include <pcl/features/normal_3d_omp.h>
#include <pcl/features/integral_image_normal.h>
#include <pcl/surface/poisson.h>

#include <pcl/sample_consensus/ransac.h>
#include <pcl/sample_consensus/sac_model_plane.h>

#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/RegionOfInterest.h>
#include <uav_detect/DetectionParamsConfig.h>

using namespace cv;
using namespace std;
using namespace uav_detect;

// shortcut type to the dynamic reconfigure manager template instance
typedef mrs_lib::DynamicReconfigureMgr<uav_detect::DetectionParamsConfig> drmgr_t;
typedef pcl::PointCloud<pcl::PointXYZ> PC;

namespace uav_detect
{

  float distsq_from_origin(const pcl::PointXYZ& point)
  {
    return point.x*point.x + point.y*point.y + point.z*point.z;
  }

  bool scaled_dist_thresholding(const pcl::PointXYZ& point_a, const pcl::PointXYZ& point_b, float squared_distance)
  {
    static const float thresh = 0.25*0.25; // metres squared
    const float d_a = distsq_from_origin(point_a);
    const float d_b = distsq_from_origin(point_b);
    const float d_max = std::max(d_a, d_b);
    const float scaled_thresh = thresh*sqrt(d_max);
    return squared_distance < scaled_thresh;
  }

  class PCLDetector : public nodelet::Nodelet
  {
  public:

    /* onInit() method //{ */
    void onInit()
    {
      ros::NodeHandle nh = nodelet::Nodelet::getMTPrivateNodeHandle();

      m_node_name = "PCLDetector";

      /* Load parameters from ROS //{*/
      NODELET_INFO("Loading default dynamic parameters:");
      m_drmgr_ptr = make_unique<drmgr_t>(nh, m_node_name);

      mrs_lib::ParamLoader pl(nh, m_node_name);
      // LOAD STATIC PARAMETERS
      NODELET_INFO("Loading static parameters:");
      pl.load_param("world_frame", m_world_frame);
      pl.load_param("filtering_leaf_size", m_drmgr_ptr->config.filtering_leaf_size);
      pl.load_param("active_box_size", m_drmgr_ptr->config.active_box_size);
      pl.load_param("exclude_box/offset/x", m_exclude_box_offset_x);
      pl.load_param("exclude_box/offset/y", m_exclude_box_offset_y);
      pl.load_param("exclude_box/offset/z", m_exclude_box_offset_z);
      pl.load_param("exclude_box/size/x", m_exclude_box_size_x);
      pl.load_param("exclude_box/size/y", m_exclude_box_size_y);
      pl.load_param("exclude_box/size/z", m_exclude_box_size_z);
      pl.load_param("keep_pc_organized", m_keep_pc_organized, false);

      // LOAD DYNAMIC PARAMETERS
      // CHECK LOADING STATUS
      if (!pl.loaded_successfully())
      {
        NODELET_ERROR("Some compulsory parameters were not loaded successfully, ending the node");
        ros::shutdown();
      }
      //}

      /* Create publishers and subscribers //{ */
      // Initialize transform listener
      m_tf_listener_ptr = std::make_unique<tf2_ros::TransformListener>(m_tf_buffer);
      // Initialize subscribers
      mrs_lib::SubscribeMgr smgr(nh, m_node_name);
      const bool subs_time_consistent = false;
      m_pc_sh = smgr.create_handler_threadsafe<PC::ConstPtr, subs_time_consistent>("pc", 1, ros::TransportHints().tcpNoDelay(), ros::Duration(5.0));
      // Initialize publishers
      /* m_detections_pub = nh.advertise<uav_detect::Detections>("detections", 10); */ 
      /* m_detected_blobs_pub = nh.advertise<uav_detect::BlobDetections>("blob_detections", 1); */
      m_global_pc_pub = nh.advertise<sensor_msgs::PointCloud2>("global_pc", 1);
      m_filtered_input_pc_pub = nh.advertise<sensor_msgs::PointCloud2>("filterd_input_pc", 1);
      //}

      m_last_detection_id = 0;

      m_det_blobs = 0;
      m_images_processed = 0;
      m_avg_fps = 0.0f;
      m_avg_delay = 0.0f;

      /* m_detector = dbd::PCLBlobDetector(m_drmgr_ptr->config, m_unknown_pixel_value); */

      m_main_loop_timer = nh.createTimer(ros::Rate(1000), &PCLDetector::main_loop, this);
      /* m_info_loop_timer = nh.createTimer(ros::Rate(1), &PCLDetector::info_loop, this); */

      m_cloud_global = boost::make_shared<pcl::PointCloud<pcl::PointNormal>>();
      m_cloud_global->header.frame_id = m_world_frame;

      cout << "----------------------------------------------------------" << std::endl;

    }
    //}

  private:
    pcl::PointCloud<pcl::PointNormal>::Ptr m_cloud_global;

    /* main_loop() method //{ */
    void main_loop([[maybe_unused]] const ros::TimerEvent& evt)
    {
      if (m_pc_sh->new_data())
      {
        /* ros::Time start_t = ros::Time::now(); */

        NODELET_INFO_STREAM("[PCLDetector]: Processing new data --------------------------------------------------------- ");

        PC::ConstPtr cloud = m_pc_sh->get_data();
        ros::Time msg_stamp;
        pcl_conversions::fromPCL(cloud->header.stamp, msg_stamp);
        NODELET_INFO_STREAM("[PCLDetector]: Input PC has " << cloud->size() << " points");
        const auto leaf_size = m_drmgr_ptr->config.filtering_leaf_size;

        /* filter input cloud and transform it to world, calculate its normals //{ */
        
        pcl::PointCloud<pcl::PointNormal>::Ptr cloud_with_normals = boost::make_shared<pcl::PointCloud<pcl::PointNormal>>();
        PC::Ptr cloud_filtered = boost::make_shared<PC>(*cloud);
        Eigen::Vector3d tf_trans;
        {
          /* pcl::PointIndices::Ptr indices_filtered = boost::make_shared<pcl::PointIndices>(); */

          /* filter by cropping points outside a box, relative to the sensor //{ */
          {
            const auto box_size = m_drmgr_ptr->config.active_box_size;
            const Eigen::Vector4f box_point1(box_size/2, box_size/2, box_size/2, 1);
            const Eigen::Vector4f box_point2(-box_size/2, -box_size/2, -box_size/2, 1);
            pcl::CropBox<pcl::PointXYZ> cb;
            cb.setMax(box_point1);
            cb.setMin(box_point2);
            cb.setInputCloud(cloud_filtered);
            cb.setNegative(false);
            cb.setKeepOrganized(m_keep_pc_organized);
            cb.filter(*cloud_filtered);
            /* cb.setNegative(false); */
            /* cb.filter(indices_filtered->indices); */
          }
          //}
          NODELET_INFO_STREAM("[PCLDetector]: Input PC after CropBox 1: " << cloud_filtered->size() << " points");
          /* NODELET_INFO_STREAM("[PCLDetector]: Input PC after CropBox 1: " << indices_filtered->indices.size() << " points"); */
          
          /* filter by cropping points inside a box, relative to the sensor //{ */
          {
            const Eigen::Vector4f box_point1(
                m_exclude_box_offset_x + m_exclude_box_size_x/2,
                m_exclude_box_offset_y + m_exclude_box_size_y/2,
                m_exclude_box_offset_z + m_exclude_box_size_z/2,
                1);
            const Eigen::Vector4f box_point2(
                m_exclude_box_offset_x - m_exclude_box_size_x/2,
                m_exclude_box_offset_y - m_exclude_box_size_y/2,
                m_exclude_box_offset_z - m_exclude_box_size_z/2,
                1);
            pcl::CropBox<pcl::PointXYZ> cb;
            cb.setMax(box_point1);
            cb.setMin(box_point2);
            cb.setInputCloud(cloud_filtered);
            cb.setNegative(true);
            cb.setKeepOrganized(m_keep_pc_organized);
            cb.filter(*cloud_filtered);
            /* cb.setInputCloud(cloud); */
            /* cb.setIndices(indices_filtered); */
            /* cb.setNegative(true); */
            /* cb.filter(indices_filtered->indices); */
          }
          //}
          NODELET_INFO_STREAM("[PCLDetector]: Input PC after CropBox 2: " << cloud_filtered->size() << " points");
          /* NODELET_INFO_STREAM("[PCLDetector]: Input PC after CropBox 2: " << indices_filtered->indices.size() << " points"); */

          /* auto pt = cloud_filtered->at(500, 3); */
          /* cloud_filtered->clear(); */
          /* cloud_filtered->push_back(pt); */
          /* cloud_filtered->push_back(pt); */
          /* cloud_filtered->height = 2; */
          /* cloud_filtered->width = 2; */
          pcl::PointCloud<pcl::Normal>::Ptr normals = boost::make_shared<pcl::PointCloud<pcl::Normal>>();
          {
            const bool debugging = m_drmgr_ptr->config.normal_debug;
            *normals = estimate_normals_organized(*cloud_filtered, *cloud, debugging);
            /* pcl::NormalEstimationOMP<pcl::PointXYZ, pcl::Normal> ne; */
            /* pcl::search::KdTree<pcl::PointXYZ>::Ptr tree = boost::make_shared<pcl::search::KdTree<pcl::PointXYZ>>(); */
            /* tree->setInputCloud(cloud_filtered); */
            /* ne.setNumberOfThreads(4); */
            /* ne.setViewPoint(0, 0, 0); */
            /* ne.setInputCloud(cloud_filtered); */
            /* ne.setSearchMethod(tree); */
            /* ne.setKSearch(5); */
            /* ne.compute(*normals);          // estimate normals */

            /* pcl::IntegralImageNormalEstimation<pcl::PointXYZ, pcl::Normal> ne; */
            /* ne.setNormalEstimationMethod(ne.AVERAGE_3D_GRADIENT); */
            /* ne.setMaxDepthChangeFactor(0.02f); */
            /* ne.setNormalSmoothingSize(10.0f); */
            /* ne.setInputCloud(cloud_filtered); */
            /* /1* ne.setIndices(indices_filtered); *1/ */
            /* /1* ne.setInputCloud(cloud); *1/ */
            /* ne.compute(*normals); */
          }

          {
            pcl::concatenateFields(*cloud_filtered, *normals, *cloud_with_normals);
            std::vector<int> unused_vec;
            pcl::removeNaNFromPointCloud(*cloud_with_normals, *cloud_with_normals, unused_vec);
            pcl::removeNaNNormalsFromPointCloud(*cloud_with_normals, *cloud_with_normals, unused_vec);
          }

          /* filter by voxel grid //{ */
          {
            /* pcl::VoxelGrid<pcl::PointXYZ> vg; */
            pcl::VoxelGrid<pcl::PointNormal> vg;
            vg.setLeafSize(leaf_size, leaf_size, leaf_size);
            /* vg.setInputCloud(cloud_filtered); */
            /* vg.filter(*cloud_filtered); */
            vg.setInputCloud(cloud_with_normals);
            vg.filter(*cloud_with_normals);
          }
          //}
          NODELET_INFO_STREAM("[PCLDetector]: Input PC after VoxelGrid: " << cloud_with_normals->size() << " points");
          /* NODELET_INFO_STREAM("[PCLDetector]: Input PC after VoxelGrid: " << indices_filtered->indices.size() << " points"); */


          Eigen::Affine3d s2w_tf;
          bool tf_ok = get_transform_to_world(cloud->header.frame_id, msg_stamp, s2w_tf);
          if (!tf_ok)
          {
            NODELET_ERROR("[PCLDetector]: Could not transform pointcloud to global, skipping.");
            return;
          }
          tf_trans = s2w_tf.translation();
          /* pcl::PointCloud<pcl::PointXYZ>::ConstPtr cloud_filtered = boost::make_shared<pcl::PointCloud<pcl::PointXYZ>>(*cloud, indices_filtered->indices); */
          /* pcl::PointCloud<pcl::Normal>::ConstPtr normals_filtered = boost::make_shared<pcl::PointCloud<pcl::Normal>>(*normals, idx_filtered->indices); */
          /* pcl::concatenateFields(*cloud_filtered, *normals, *cloud_with_normals); */
          pcl::transformPointCloud(*cloud_with_normals, *cloud_with_normals, s2w_tf.cast<float>());
          cloud_with_normals->header.frame_id = m_world_frame;
          /* tf2::doTransform(cloud, cloud, s2w_tf); */

          /* cloud = cloud_filtered; */
          NODELET_INFO_STREAM("[PCLDetector]: Filtered input PC has " << cloud_with_normals->size() << " points");
        }
        
        //}

        /* add filtered input cloud to global cloud and filter it //{ */
        
        {
          *m_cloud_global += *cloud_with_normals;

          /* filter by mutual point distance (voxel grid) //{ */
          pcl::VoxelGrid<pcl::PointNormal> vg;
          vg.setLeafSize(leaf_size, leaf_size, leaf_size);
          vg.setInputCloud(m_cloud_global);
          vg.filter(*m_cloud_global);
          //}

          /* filter by cropping points outside a box, relative to the sensor //{ */
          const auto box_size = m_drmgr_ptr->config.active_box_size;
          const Eigen::Vector4f sensor_origin(tf_trans.x(), tf_trans.y(), tf_trans.z(), 1.0f);
          const Eigen::Vector4f box_point1 = sensor_origin - Eigen::Vector4f(box_size/2, box_size/2, box_size/2, 0);
          const Eigen::Vector4f box_point2 = sensor_origin + Eigen::Vector4f(box_size/2, box_size/2, box_size/2, 0);
          pcl::CropBox<pcl::PointNormal> cb;
          cb.setMin(box_point1);
          cb.setMax(box_point2);
          cb.setInputCloud(m_cloud_global);
          cb.filter(*m_cloud_global);
          //}

          NODELET_INFO_STREAM("[PCLDetector]: Global pointcloud has " << m_cloud_global->size() << " points");
        }
        
        //}

        /* fit a surface to the global cloud and filter the edge points //{ */
        {
          pcl::PointCloud<pcl::PointNormal>::Ptr mesh_cloud = boost::make_shared<pcl::PointCloud<pcl::PointNormal>>();
          std::vector<pcl::Vertices> mesh_vertices;
          pcl::Poisson<pcl::PointNormal> poisson;
          poisson.setInputCloud(m_cloud_global);
          poisson.reconstruct(*mesh_cloud, mesh_vertices);
          m_cloud_global = mesh_cloud;
          
          /* filter by mutual point distance (voxel grid) //{ */
          pcl::VoxelGrid<pcl::PointNormal> vg;
          vg.setLeafSize(leaf_size, leaf_size, leaf_size);
          vg.setInputCloud(m_cloud_global);
          vg.filter(*m_cloud_global);
          //}
        }
        //}

        /* /1* unused //{ *1/ */
        
        /* /1* pcl::PointIndices::Ptr valid_idx = boost::make_shared<pcl::PointIndices>(); *1/ */
        /* /1* const auto& vecpts = m_cloud_global->points; *1/ */
        /* /1* for (unsigned it = 0; it < vecpts.size(); it++) *1/ */
        /* /1* { *1/ */
        /* /1*   const auto& pt = vecpts[it]; *1/ */
        /* /1*   if (distsq_from_origin(pt) > 20.0f*20.0f) *1/ */
        /* /1*     continue; *1/ */
        /* /1*   else *1/ */
        /* /1*     valid_idx->indices.push_back(it); *1/ */
        /* /1* } *1/ */
        /* /1* NODELET_INFO_STREAM("[PCLDetector]: Pointcloud after removing far points has " << valid_idx->indices.size() << " points"); *1/ */
        
        /* // Creating the KdTree object for the search method of the extraction */
        /* /1* pcl::search::KdTree<pcl::PointXYZ>::Ptr kdtree = boost::make_shared<pcl::search::KdTree<pcl::PointXYZ>>(); *1/ */
        /* /1* kdtree->setInputCloud(m_cloud_global); *1/ */
        
        /* /1* std::vector<pcl::PointIndices> cluster_indices; *1/ */
        /* /1* pcl::ConditionalEuclideanClustering<pcl::PointXYZ> ec; *1/ */
        /* /1* ec.setClusterTolerance(2.5);  // metres *1/ */
        /* /1* ec.setConditionFunction(&scaled_dist_thresholding); *1/ */
        /* /1* ec.setMinClusterSize(10); *1/ */
        /* /1* ec.setMaxClusterSize(500); *1/ */
        /* /1* /2* ec.setSearchMethod(kdtree); *2/ *1/ */
        /* /1* ec.setInputCloud(m_cloud_global); *1/ */
        /* /1* /2* ec.setIndices(valid_idx); *2/ *1/ */
        /* /1* ec.segment(cluster_indices); *1/ */
        
        /* /1* pcl::PointCloud<pcl::PointXYZL> cloud_labeled; *1/ */
        /* /1* cloud_labeled.reserve(m_cloud_global->size()); *1/ */
        /* /1* /2* cloud_labeled.width = n_pts; *2/ *1/ */
        /* /1* /2* cloud_labeled.height = 1; *2/ *1/ */
        /* /1* /2* cloud_labeled.is_dense = true; *2/ *1/ */
        /* /1* int label = 0; *1/ */
        /* /1* for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin(); it != cluster_indices.end(); ++it) *1/ */
        /* /1* { *1/ */
        /* /1*   pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_cluster = boost::make_shared<pcl::PointCloud<pcl::PointXYZ>>(); *1/ */
        /* /1*   for (std::vector<int>::const_iterator pit = it->indices.begin(); pit != it->indices.end(); ++pit) *1/ */
        /* /1*   { *1/ */
        /* /1*     const auto& pt = m_cloud_global->points[*pit]; *1/ */
        /* /1*     pcl::PointXYZL ptl; *1/ */
        /* /1*     ptl.x = pt.x; *1/ */
        /* /1*     ptl.y = pt.y; *1/ */
        /* /1*     ptl.z = pt.z; *1/ */
        /* /1*     ptl.label = label; *1/ */
        /* /1*     cloud_labeled.points.push_back(ptl); *1/ */
        /* /1*   } *1/ */
        /* /1*   label++; *1/ */
        /* /1* } *1/ */
        /* /1* NODELET_INFO_STREAM("[PCLDetector]: Extracted " << label << " clusters"); *1/ */
        /* /1* NODELET_INFO_STREAM("[PCLDetector]: Processed pointcloud has " << cloud_labeled.size() << " points"); *1/ */
        
        /* //} */

        m_cloud_global->header.stamp = cloud->header.stamp;

        if (m_filtered_input_pc_pub.getNumSubscribers() > 0)
          m_filtered_input_pc_pub.publish(cloud_with_normals);

        if (m_global_pc_pub.getNumSubscribers() > 0)
          m_global_pc_pub.publish(m_cloud_global);

        NODELET_INFO_STREAM("[PCLDetector]: Done processing data --------------------------------------------------------- ");
      }
    }
    //}

    /* /1* info_loop() method //{ *1/ */
    /* void info_loop([[maybe_unused]] const ros::TimerEvent& evt) */
    /* { */
    /*   const float dt = (evt.current_real - evt.last_real).toSec(); */
    /*   std::lock_guard<std::mutex> lck(m_stat_mtx); */
    /*   const float blobs_per_image = m_det_blobs/float(m_images_processed); */
    /*   const float input_fps = m_images_processed/dt; */
    /*   NODELET_INFO_STREAM("[" << m_node_name << "]: det. blobs/image: " << blobs_per_image << " | inp. FPS: " << round(input_fps) << " | proc. FPS: " << round(m_avg_fps) << " | delay: " << round(1000.0f*m_avg_delay) << "ms"); */
    /*   m_det_blobs = 0; */
    /*   m_images_processed = 0; */
    /* } */
    /* //} */

  private:

    /* estimate_normals_organized() method //{ */
    // THESE VALUES HAVE TO CORRESPOND TO THE DYNAMIC RECONFIGURE ENUM
    enum plane_fit_method_t
    {
      RANSAC = 0,
      SVD = 1,
    };

    pcl::PointCloud<pcl::Normal> estimate_normals_organized(PC& pc, const PC& unfiltered_pc, const bool debugging = false)
    {
      pcl::PointCloud<pcl::Normal> normals;
      const auto neighborhood_rows = m_drmgr_ptr->config.normal_neighborhood_rows;
      const auto neighborhood_cols = m_drmgr_ptr->config.normal_neighborhood_cols;
      const plane_fit_method_t fitting_method = (plane_fit_method_t)m_drmgr_ptr->config.normal_method;
    
      if (debugging)
      {
        const auto i = std::clamp(m_drmgr_ptr->config.normal_debug_col, 0, int(pc.width)-1);
        const auto j = std::clamp(m_drmgr_ptr->config.normal_debug_row, 0, int(pc.height)-1);
        const pcl::Normal n = estimate_normal(i, j, pc, unfiltered_pc, neighborhood_rows, neighborhood_cols, fitting_method, debugging);
        for (unsigned it = 0; it < pc.size(); it++)
          normals.push_back(n);
        normals.width = pc.width;
        normals.height = pc.height;
      } else
      {
        normals.resize(pc.size());
        normals.width = pc.width;
        normals.height = pc.height;
        for (unsigned i = 0; i < pc.width; i++)
          for (unsigned j = 0; j < pc.height; j++)
          {
            const pcl::Normal n = estimate_normal(i, j, pc, unfiltered_pc, neighborhood_rows, neighborhood_cols, fitting_method, debugging);
            normals.at(i, j) = n;
          }
      }
    
      normals.is_dense = pc.is_dense;
      normals.header = pc.header;
      return normals;
    }

    template <class Point_T>
    bool valid_pt(Point_T pt)
    {
      return (std::isfinite(pt.x) &&
              std::isfinite(pt.y) &&
              std::isfinite(pt.z));
    }

    using plane_params_t = Eigen::Vector4f;
    constexpr static float nan = std::numeric_limits<float>::quiet_NaN();
    plane_params_t fit_plane(PC::ConstPtr pcl)
    {
      const static plane_params_t invalid_plane_params = plane_params_t(nan, nan, nan, nan);
      if (pcl->size() < 3)
        return invalid_plane_params;

      Eigen::Matrix<float, 3, -1> points = pcl->getMatrixXfMap(3, 4, 0);
      /* cout << "Fitting plane to points:" << std::endl << points << std::endl; */
      const Eigen::Vector3f centroid = points.rowwise().mean();
      points.colwise() -= centroid;
      const auto svd = points.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV);
      const Eigen::Vector3f normal = svd.matrixU().rightCols<1>().normalized();
      const double d = normal.dot(centroid);
      const plane_params_t ret(normal.x(), normal.y(), normal.z(), d);
      return ret;
    }

    plane_params_t fit_plane_RANSAC(PC::ConstPtr pcl)
    {
      const static plane_params_t invalid_plane_params = plane_params_t(nan, nan, nan, nan);
      pcl::SampleConsensusModelPlane<pcl::PointXYZ>::Ptr model_p
        = boost::make_shared<pcl::SampleConsensusModelPlane<pcl::PointXYZ>>(pcl, true);
      if (pcl->size() < model_p->getSampleSize())
        return invalid_plane_params;

      pcl::RandomSampleConsensus<pcl::PointXYZ> ransac(model_p);
      ransac.setDistanceThreshold(m_drmgr_ptr->config.normal_threshold);
      ransac.setMaxIterations(m_drmgr_ptr->config.normal_iterations);
      ransac.setProbability(m_drmgr_ptr->config.normal_probability);
      if (ransac.computeModel())
      {
        Eigen::VectorXf coeffs;
        ransac.getModelCoefficients(coeffs);
        // normalize the normal (lel)
        const double c = coeffs.block<3, 1>(0, 0).norm();
        coeffs = coeffs/c;
        return coeffs;
      } else
      {
        return invalid_plane_params;
      }
    }

    pcl::Normal estimate_normal(const int col, const int row, PC& pc, const PC& unfiltered_pc, const int neighborhood_rows, const int neighborhood_cols, const plane_fit_method_t method, const bool debugging = false)
    {
      /* cout << "Using neighborhood: " << neighborhood << std::endl; */
      const static pcl::Normal invalid_normal(nan, nan, nan);
      const auto pt = pc.at(col, row);
      if (!valid_pt(pt) && !debugging)
        return invalid_normal;

      const int col_bot = std::max(col - neighborhood_cols, 0);
      const int col_top = std::min(col + neighborhood_cols, (int)pc.width-1);
      const int row_bot = std::max(row - neighborhood_rows, 0);
      const int row_top = std::min(row + neighborhood_rows, (int)pc.height-1);
      /* cout << "bounds: [" << col_bot << ", " << col_top << "]; [" << row_bot << ", " << row_top << "]" << std::endl; */
      PC::Ptr neig_pc = boost::make_shared<PC>();
      neig_pc->reserve((2*neighborhood_cols+1)*(2*neighborhood_rows+1));
      for (int i = col_bot; i <= col_top; i++)
        for (int j = row_bot; j <= row_top; j++)
        {
          const auto pt = unfiltered_pc.at(i, j);
          if (valid_pt(pt))
            neig_pc->push_back(pt);
        }

      if (debugging)
        pc = *neig_pc;
      /* cout << "Neighborhood points size: " << neig_pc->size() << std::endl; */
      plane_params_t plane_params;
      switch (method)
      {
        case plane_fit_method_t::RANSAC:
          plane_params = fit_plane_RANSAC(neig_pc);
          break;
        case plane_fit_method_t::SVD:
          plane_params = fit_plane(neig_pc);
          break;
        default:
          ROS_ERROR("[PCLDetector]: Unknown plane fitting method: %d! Skipping.", method);
          plane_params = plane_params_t(nan, nan, nan, nan);
          break;
      }
      Eigen::Vector3f normal_vec = plane_params.block<3, 1>(0, 0);
      if (!normal_vec.array().isFinite().all())
        return invalid_normal;

      const Eigen::Vector3f camera_vec = -Eigen::Vector3f(pt.x, pt.y, pt.z).normalized();
      /* const Eigen::Vector3f camera_vec = -Eigen::Vector3f(0, 0, 1).normalized(); */
      const auto ancos = normal_vec.dot(camera_vec);
      if (ancos < 0.0)
      {
        /* cout << "do flipping normal " << coeffs << " to correspond to " << camera_vec << " (dot product is " << dprod << ")" << std::endl; */
        normal_vec = -normal_vec;
      }
      /* else */
      /* { */
      /*   cout << "not flipping normal " << coeffs << " to correspond to " << camera_vec << " (dot product is " << dprod << ")" << std::endl; */
      /* } */
      const pcl::Normal ret(normal_vec(0), normal_vec(1), normal_vec(2));
      /* cout << "Camera: [" << std::endl << camera_vec.transpose() << std::endl << "]" << std::endl; */
      /* cout << "Normal: [" << std::endl << normal_vec.transpose() << std::endl << "], acos: " << ancos << std::endl; */
      return ret;
    }

    //}

    /* get_transform_to_world() method //{ */
    bool get_transform_to_world(const string& frame_id, ros::Time stamp, Eigen::Affine3d& tf_out) const
    {
      try
      {
        const ros::Duration timeout(1.0 / 100.0);
        // Obtain transform from sensor into world frame
        geometry_msgs::TransformStamped transform;
        transform = m_tf_buffer.lookupTransform(m_world_frame, frame_id, stamp, timeout);
        tf_out = tf2::transformToEigen(transform.transform);
      }
      catch (tf2::TransformException& ex)
      {
        NODELET_WARN_THROTTLE(1.0, "[%s]: Error during transform from \"%s\" frame to \"%s\" frame.\n\tMSG: %s", m_node_name.c_str(), frame_id.c_str(),
                          m_world_frame.c_str(), ex.what());
        return false;
      }
      return true;
    }
    //}

  private:

    // --------------------------------------------------------------
    // |                ROS-related member variables                |
    // --------------------------------------------------------------

    /* ROS related variables (subscribers, timers etc.) //{ */
    std::unique_ptr<drmgr_t> m_drmgr_ptr;
    tf2_ros::Buffer m_tf_buffer;
    std::unique_ptr<tf2_ros::TransformListener> m_tf_listener_ptr;
    mrs_lib::SubscribeHandlerPtr<PC::ConstPtr> m_pc_sh;
    /* ros::Publisher m_detections_pub; */
    ros::Publisher m_global_pc_pub;
    ros::Publisher m_filtered_input_pc_pub;
    ros::Timer m_main_loop_timer;
    ros::Timer m_info_loop_timer;
    std::string m_node_name;
    //}

  private:

    // --------------------------------------------------------------
    // |                 Parameters, loaded from ROS                |
    // --------------------------------------------------------------

    /* Parameters, loaded from ROS //{ */
    
    std::string m_world_frame;
    double m_exclude_box_offset_x;
    double m_exclude_box_offset_y;
    double m_exclude_box_offset_z;
    double m_exclude_box_size_x;
    double m_exclude_box_size_y;
    double m_exclude_box_size_z;
    bool m_keep_pc_organized;
    
    //}

  private:

    // --------------------------------------------------------------
    // |                   Other member variables                   |
    // --------------------------------------------------------------

    uint32_t m_last_detection_id;
    
    /* Statistics variables //{ */
    std::mutex m_stat_mtx;
    unsigned   m_det_blobs;
    unsigned   m_images_processed;
    float      m_avg_fps;
    float      m_avg_delay;
    //}

  }; // class PCLDetector
}; // namespace uav_detect

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(uav_detect::PCLDetector, nodelet::Nodelet)
