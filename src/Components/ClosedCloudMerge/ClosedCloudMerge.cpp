//
#include "ClosedCloudMerge.hpp"
#include "Common/Logger.hpp"

#include <Types/MergeUtils.hpp>
#include <boost/bind.hpp>
///////////////////////////////////////////
#include <string>
#include <cmath>
#include <set>
#include <pcl/filters/extract_indices.h>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/point_representation.h>
//
#include "pcl/impl/instantiate.hpp"
#include "pcl/search/kdtree.h"
#include "pcl/search/impl/kdtree.hpp"
#include <pcl/registration/correspondence_estimation.h>
#include "pcl/registration/correspondence_rejection_sample_consensus.h"
#include <pcl/registration/lum.h>

#include <pcl/io/pcd_io.h>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/visualization/point_cloud_color_handlers.h>
////////////////////////////////////////////////////////////////////////
#include <pcl/filters/filter.h>

#include <pcl/filters/voxel_grid.h>
#include <pcl/features/normal_3d.h>

#include <pcl/registration/distances.h>

#include <pcl/keypoints/uniform_sampling.h>

namespace Processors {
namespace ClosedCloudMerge {

class SHOTonlyXYZRepresentation: public pcl::DefaultFeatureRepresentation<PointXYZSHOT> {
	/// Templatiated number of SIFT descriptor dimensions.
	using pcl::PointRepresentation<PointXYZSHOT>::nr_dimensions_;

public:
	SHOTonlyXYZRepresentation() {
		// Define the number of dimensions.
		nr_dimensions_ = 3;
		trivial_ = false;
	}

	/// Overrides the copyToFloatArray method to define our feature vector
	virtual void copyToFloatArray(const PointXYZSHOT &p, float * out) const {
		out[0] = p.x;
		out[1] = p.y;
		out[2] = p.z;
	}
};

class SHOTonlyDescriptorRepresentation: public pcl::DefaultFeatureRepresentation<PointXYZSHOT> {
	/// Templatiated number of SIFT descriptor dimensions.
	using pcl::PointRepresentation<PointXYZSHOT>::nr_dimensions_;

public:
	SHOTonlyDescriptorRepresentation() {
		// Define the number of dimensions.
		nr_dimensions_ = 352;
		trivial_ = false;
	}

	/// Overrides the copyToFloatArray method to define our feature vector
	virtual void copyToFloatArray(const PointXYZSHOT &p, float * out) const {
		for (int i = 0; i < 352; ++i) {
			out[i] = p.descriptor[i];
		}
	}
};

void computeCorrespondences(const pcl::PointCloud<PointXYZSHOT>::ConstPtr &cloud_src,
		const pcl::PointCloud<PointXYZSHOT>::ConstPtr &cloud_trg, const pcl::CorrespondencesPtr& correspondences) {
	pcl::registration::CorrespondenceEstimation<PointXYZSHOT, PointXYZSHOT> correst;
	SHOTonlyDescriptorRepresentation::Ptr point_representation(new SHOTonlyDescriptorRepresentation());
	correst.setPointRepresentation(point_representation);
	correst.setInputSource(cloud_src);
	correst.setInputTarget(cloud_trg);
	// Find correspondences.
	correst.determineReciprocalCorrespondences(*correspondences);

}

ClosedCloudMerge::ClosedCloudMerge(const std::string & name) :
		Base::Component(name), prop_ICP_alignment("ICP.Points", true), prop_ICP_alignment_normal("ICP.Normals", true), prop_ICP_alignment_color(
				"ICP.Color", false), ICP_transformation_epsilon("ICP.Tranformation_epsilon", 1e-6), ICP_max_correspondence_distance(
				"ICP.Correspondence_distance", 0.1), ICP_max_iterations("ICP.Iterations", 2000), RanSAC_inliers_threshold(
				"RanSac.Inliers_threshold", 0.01f), RanSAC_max_iterations("RanSac.Iterations", 2000), threshold(
				"threshold", 23), maxIterations("Interations.Max", 5) {
	registerProperty(prop_ICP_alignment);
	registerProperty(prop_ICP_alignment_normal);
	registerProperty(prop_ICP_alignment_color);
	registerProperty(ICP_transformation_epsilon);
	registerProperty(ICP_max_correspondence_distance);
	registerProperty(ICP_max_iterations);
	registerProperty(RanSAC_inliers_threshold);
	registerProperty(RanSAC_max_iterations);
	registerProperty(maxIterations);
	registerProperty(threshold);

	properties.ICP_transformation_epsilon = ICP_transformation_epsilon;
	properties.ICP_max_iterations = ICP_max_iterations;
	properties.ICP_max_correspondence_distance = ICP_max_correspondence_distance;
	properties.RanSAC_inliers_threshold = RanSAC_inliers_threshold;
	properties.RanSAC_max_iterations = RanSAC_max_iterations;
}

ClosedCloudMerge::~ClosedCloudMerge() {
}


void ClosedCloudMerge::prepareInterface() {
	// Register data streams.
	registerStream("in_cloud_xyzrgb", &in_cloud_xyzrgb);
	registerStream("in_cloud_xyzrgb_normals", &in_cloud_xyzrgb_normals);
	registerStream("in_cloud_xyzsift", &in_cloud_xyzsift);
	registerStream("in_cloud_xyzshot", &in_cloud_xyzshot);
	registerStream("out_instance", &out_instance);
	registerStream("out_cloud_xyzrgb", &out_cloud_xyzrgb);
	registerStream("out_cloud_xyzrgb_normals", &out_cloud_xyzrgb_normals);
	registerStream("out_cloud_xyzsift", &out_cloud_xyzsift);
	registerStream("out_cloud_xyzshot", &out_cloud_xyzshot);
	registerStream("out_mean_viewpoint_features_number", &out_mean_viewpoint_features_number);

	// Register single handler - the "addViewToModel" function.
	h_addViewToModel.setup(boost::bind(&ClosedCloudMerge::addViewToModel, this));
	registerHandler("addViewToModel", &h_addViewToModel);
	addDependency("addViewToModel", &in_cloud_xyzsift);
	addDependency("addViewToModel", &in_cloud_xyzshot);
	addDependency("addViewToModel", &in_cloud_xyzrgb);
	addDependency("addViewToModel", &in_cloud_xyzrgb_normals);
}

bool ClosedCloudMerge::onInit() {
	CLOG(LDEBUG)<< "onInit";
	// Number of viewpoints.
	counter = 0;
	// Mean number of features per view.
	mean_viewpoint_features_number = 0;

	global_trans = Eigen::Matrix4f::Identity();

	cloud_merged = pcl::PointCloud<pcl::PointXYZRGB>::Ptr(new pcl::PointCloud<pcl::PointXYZRGB>());
	cloud_sift_merged = pcl::PointCloud<PointXYZSIFT>::Ptr(new pcl::PointCloud<PointXYZSIFT>());
	cloud_shot_merged = pcl::PointCloud<PointXYZSHOT>::Ptr(new pcl::PointCloud<PointXYZSHOT>());
	cloud_normal_merged = pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr(new pcl::PointCloud<pcl::PointXYZRGBNormal>());
	CLOG(LDEBUG)<< "onInit";
	return true;
}

bool ClosedCloudMerge::onFinish() {
	return true;
}

bool ClosedCloudMerge::onStop() {
	return true;
}

bool ClosedCloudMerge::onStart() {
	CLOG(LDEBUG)<< "onStart";
	return true;
}

void ClosedCloudMerge::addViewToModel() {
	CLOG(LDEBUG)<< "addViewToModel";

	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudrgb = in_cloud_xyzrgb.read();
	pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr cloud = in_cloud_xyzrgb_normals.read();
	pcl::PointCloud<PointXYZSIFT>::Ptr cloud_sift = in_cloud_xyzsift.read();
	pcl::PointCloud<PointXYZSHOT>::Ptr cloud_shot = in_cloud_xyzshot.read();

	// TODO if empty()

	CLOG(LINFO) << "addViewToModel incoming cloud_xyzrgb size: "<<cloudrgb->size();
	CLOG(LINFO) << "addViewToModel incoming cloud_xyzrgb_normals size: "<<cloud->size();
	CLOG(LINFO) << "addViewToModel incoming cloud_xyzsift size: "<<cloud_sift->size();
	CLOG(LINFO) << "addViewToModel incoming cloud_xyzshot size: "<<cloud_shot->size();

	// Remove NaNs.
	std::vector<int> indices;
	cloud->is_dense = false;
	pcl::removeNaNFromPointCloud(*cloud, *cloud, indices);
	cloud_sift->is_dense = false;
	pcl::removeNaNFromPointCloud(*cloud_sift, *cloud_sift, indices);

	CLOG(LDEBUG) << "addViewToModel cloud_xyzrgb size without NaN: "<<cloud->size();
	CLOG(LDEBUG) << "addViewToModel cloud_xyzsift size without NaN: "<<cloud_sift->size();

	CLOG(LINFO) << "view number: "<<counter;

	rgb_views.push_back(pcl::PointCloud<pcl::PointXYZRGB>::Ptr (new pcl::PointCloud<pcl::PointXYZRGB>()));
	shot_views.push_back(pcl::PointCloud<PointXYZSHOT>::Ptr (new pcl::PointCloud<PointXYZSHOT>()));
	rgbn_views.push_back(pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr (new pcl::PointCloud<pcl::PointXYZRGBNormal>()));

	counter++;

// First cloud.
	if (counter == 1) {
		//counter++;
		//	mean_viewpoint_features_number = cloud_sift->size();
		// Push results to output data ports.

		// TODO
		out_mean_viewpoint_features_number.write(cloud_sift->size());

		lum_sift.addPointCloud(cloud_sift);
		*rgbn_views[0] = *cloud;
		*rgb_views[0] = *cloudrgb;
		*shot_views[0] = *cloud_shot;

		/*		rgb_views.push_back(cloudrgb);// index == 0
		 shot_views.push_back(cloud_shot);
		 rgbn_views.push_back(cloud);*/

		*cloud_merged = *cloudrgb;
		*cloud_normal_merged = *cloud;
		*cloud_sift_merged = *cloud_sift;
		*cloud_shot_merged = *cloud_shot;

		/*
		 pcl::copyPointCloud (*cloud_merged, *cloudrgb);
		 pcl::copyPointCloud (*cloud_normal_merged, *cloud);
		 pcl::copyPointCloud (*cloud_sift_merged, *cloud_sift);
		 pcl::copyPointCloud (*cloud_shot_merged, *cloud_shot);
		 */

		out_cloud_xyzrgb.write(cloudrgb);
		out_cloud_xyzrgb_normals.write(cloud);
		out_cloud_xyzsift.write(cloud_sift);
		out_cloud_xyzshot.write(cloud_shot);

		CLOG(LINFO) << "first clouds - return";

	}
//	 Find corespondences between feature clouds.
//	 Initialize parameters.
	pcl::CorrespondencesPtr correspondences(new pcl::Correspondences());
	MergeUtils::computeCorrespondences(cloud_sift, cloud_sift_merged, correspondences);

	// Compute transformation between clouds and SOMGenerator global transformation of cloud.
	pcl::Correspondences inliers;
	Eigen::Matrix4f current_trans = MergeUtils::computeTransformationSAC(cloud_sift, cloud_sift_merged, correspondences, inliers, properties);
	if (current_trans == Eigen::Matrix4f::Identity())
	{
		CLOG(LINFO) << "addViewToModel cloud couldn't be merged";

		CLOG(LINFO) << "SENDING ... model cloud_merged->size(): "<< cloud_merged->size();
		CLOG(LINFO) << "SENDING ... model cloud_normal_merged->size(): "<< cloud_normal_merged->size();
		CLOG(LINFO) << "SENDING ... model cloud_sift_merged->size(): "<< cloud_sift_merged->size();
		CLOG(LINFO) << "SENDING ... model cloud_shot_merged->size(): "<< cloud_shot_merged->size();

		counter--;
		out_cloud_xyzrgb.write(cloud_merged);
		out_cloud_xyzrgb_normals.write(cloud_normal_merged);
		out_cloud_xyzsift.write(cloud_sift_merged);
		out_cloud_xyzshot.write(cloud_shot_merged);
		// Push SOM - depricated.
//		out_instance.write(produce());
		return;
	}

	pcl::transformPointCloud(*cloud, *cloud, current_trans);
	pcl::transformPointCloud(*cloudrgb, *cloudrgb, current_trans);
	pcl::transformPointCloud(*cloud_sift, *cloud_sift, current_trans);
	pcl::transformPointCloud(*cloud_shot, *cloud_shot, current_trans);

	current_trans = MergeUtils::computeTransformationIPCNormals(cloud, cloud_normal_merged, properties);
//
	pcl::transformPointCloud(*cloud, *cloud, current_trans);
	pcl::transformPointCloud(*cloudrgb, *cloudrgb, current_trans);
	pcl::transformPointCloud(*cloud_sift, *cloud_sift, current_trans);

	lum_sift.addPointCloud(cloud_sift);
	// poprzednie chmury
	*rgbn_views[counter -1] = *cloud;
	*rgb_views[counter -1] = *cloudrgb;
	*shot_views[counter-1] = *cloud_shot;

	CLOG(LTRACE) << " ADD RGB_VIEW SIZE :" << cloudrgb->size() << "INDEX :" << counter-1;

	/*
	 rgb_views.push_back(cloudrgb);// index == counter - 1
	 shot_views.push_back(cloud_shot);
	 rgbn_views.push_back(cloud);*/

	int added = 0;
	for (int i = counter - 2; i >= 0; i--)
	{
		pcl::CorrespondencesPtr correspondences2(new pcl::Correspondences());
		MergeUtils::computeCorrespondences(lum_sift.getPointCloud(counter - 1), lum_sift.getPointCloud(i), correspondences2);
		pcl::CorrespondencesPtr correspondences3(new pcl::Correspondences());
		MergeUtils::computeTransformationSAC(lum_sift.getPointCloud(counter - 1), lum_sift.getPointCloud(i), correspondences2, *correspondences3, properties);
		//cortab[counter-1][i] = inliers2;
		CLOG(LINFO) << "  correspondences3: " << correspondences3->size() << " out of " << correspondences2->size();
		if (correspondences3->size() > 8) {
			lum_sift.setCorrespondences(counter-1, i, correspondences3);
			added++;
			for(int j = 0; j< correspondences3->size();j++) {
				if (correspondences3->at(j).index_query >=lum_sift.getPointCloud(counter - 1)->size() || correspondences3->at(j).index_match >=lum_sift.getPointCloud(i)->size()) {
					continue;
				}
				if (lum_sift.getPointCloud(i)->at(correspondences3->at(j).index_match).multiplicity != -1) {
					lum_sift.getPointCloud(counter - 1)->at(correspondences3->at(j).index_query).multiplicity = lum_sift.getPointCloud(i)->at(correspondences3->at(j).index_match).multiplicity + 1;
					lum_sift.getPointCloud(i)->at(correspondences3->at(j).index_match).multiplicity=-1;
				} else
				lum_sift.getPointCloud(counter - 1)->at(correspondences3->at(j).index_query).multiplicity += 1;
			}

		}
		//break;
		//CLOG(LINFO) << "computet for "<<counter-1 <<" and "<< i << "  correspondences2: " << correspondences2->size() << " out of " << correspondences2->size();
	}

	CLOG(LINFO) << "cloud_merged from LUM ";

	*cloud_merged = *(rgb_views[0]);
	//pcl::copyPointCloud(*(rgb_views[0]), *cloud_merged);
	//cloud_normal_merged = rgbn_views[0];
	//cloud_shot_merged = shot_views[0];
//	pcl::copyPointCloud(*(shot_views[0]), *cloud_shot_merged);

	CLOG(LINFO) << "addViewToModel 1. cloud_merged size: "<<cloud_merged->size();
	CLOG(LINFO) << "addViewToModel 1. cloud_normal_merged size: "<<cloud_normal_merged->size();
	CLOG(LINFO) << "addViewToModel 1. cloud_shot_merged size: "<<cloud_shot_merged->size();

	if (false) {
		CLOG(LINFO) << "\n\n\n\nTHE END\n\n\n\n";

	} else if (counter > threshold) {
		lum_sift.setMaxIterations(maxIterations);
		lum_sift.compute();
		cloud_sift_merged = lum_sift.getConcatenatedCloud ();
		CLOG(LINFO) << "ended";
		CLOG(LINFO) << "cloud_merged from LUM ";
		for (int i = 1; i < threshold; i++)
		{

			pcl::PointCloud<pcl::PointXYZRGB> tmprgb = *(rgb_views[i]);
			//		pcl::PointCloud<pcl::PointXYZRGBNormal> tmp = *(rgbn_views[i]);
			//		pcl::transformPointCloud(tmp, tmp, lum_sift.getTransformation (i));
			pcl::transformPointCloud(tmprgb, tmprgb, lum_sift.getTransformation (i));
			*cloud_merged += tmprgb;
			//		*cloud_normal_merged += tmp;

			pcl::PointCloud<PointXYZSHOT>::Ptr temp_shots = shot_views[i];
			pcl::transformPointCloud(*temp_shots, *temp_shots, lum_sift.getTransformation (i));

			/*
			 pcl::PointCloud<pcl::PointXYZRGB> tmprgb = *(rgb_views[i]);
			 pcl::copyPointCloud(*(rgb_views[i]), tmprgb);
			 //	pcl::PointCloud<pcl::PointXYZRGBNormal> tmp = *(rgbn_views[i]);
			 //	pcl::transformPointCloud(tmp, tmp, lum_sift.getTransformation (i));
			 pcl::transformPointCloud(tmprgb, tmprgb, lum_sift.getTransformation (i));
			 *cloud_merged += tmprgb;
			 //	*cloud_normal_merged += tmp;

			 pcl::PointCloud<PointXYZSHOT>::Ptr temp_shots = shot_views[i];
			 */

			CLOG(LINFO) << "addViewToModel tmprgb size: "<<tmprgb.size()<<" for " << i;
			//		CLOG(LINFO) << "addViewToModel tmp size: "<<tmp.size()<<" for " << i;
			CLOG(LINFO) << "addViewToModel temp_shots size: "<<temp_shots->size() <<" for " << i;

			int removed = 0;

			pcl::CorrespondencesPtr correspondences_shot(new pcl::Correspondences());
			computeCorrespondences(temp_shots, cloud_shot_merged, correspondences_shot);

			CLOG(LINFO) << "shot correspondences: " << correspondences_shot->size();
			for(int j = 0; j< correspondences_shot->size();j++) {
				//	CLOG(LINFO) << "POINT 3";

				pcl::Correspondence corr = correspondences_shot->at(j);
				//	CLOG(LINFO) << "POINT 4";
				//	CLOG(LINFO) << "temp_shots" << temp_shots->size();
				///	CLOG(LINFO) << "cloud_shot_merged" << cloud_shot_merged->size();
				//	CLOG(LINFO) << "index_query" << corr.index_query;
				//	CLOG(LINFO) << "index_match" << corr.index_match;

				double dist = pcl::distances::l2(
						temp_shots->at(corr.index_query).getVector4fMap(),
						cloud_shot_merged->at(corr.index_match).getVector4fMap()

				);

				//	CLOG(LINFO) << "POINT 5";

				// TODO parametryzacja

				CLOG(LINFO) << "DIST: " << dist;

				if (dist < 0.01) {
					temp_shots->at(corr.index_query).multiplicity = cloud_shot_merged->at(corr.index_match).multiplicity + 1;
					cloud_shot_merged->at(corr.index_match).multiplicity =-1;
					removed ++;
				}
			}
			pcl::PointCloud<PointXYZSHOT>::iterator pt_iter_shot = cloud_shot_merged->begin();
			while(pt_iter_shot!=cloud_shot_merged->end()) {
				if(pt_iter_shot->multiplicity==-1) {
					pt_iter_shot = cloud_shot_merged->erase(pt_iter_shot);
				} else {
					++pt_iter_shot;
				}
			}

			CLOG(LINFO) << "addViewToModel removed size: "<<removed;

			*cloud_shot_merged += *temp_shots;

		}

		// Delete points.
		pcl::PointCloud<PointXYZSIFT>::iterator pt_iter = cloud_sift_merged->begin();
		while(pt_iter!=cloud_sift_merged->end()) {
			if(pt_iter->multiplicity==-1) {
				pt_iter = cloud_sift_merged->erase(pt_iter);
			} else {
				++pt_iter;
			}
		}

		CLOG(LINFO) << "addViewToModel after all cloud_merged size: "<<cloud_merged->size();
		CLOG(LINFO) << "addViewToModel after all cloud_normal_merged size: "<<cloud_normal_merged->size();
		CLOG(LINFO) << "addViewToModel after all cloud_shot_merged size: "<<cloud_shot_merged->size();

	} else { // counter <= threshold
		for (int i = 1; i < counter; i++)
		{
			pcl::PointCloud<pcl::PointXYZRGB> tmprgb = *(rgb_views[i]);
			pcl::PointCloud<pcl::PointXYZRGBNormal> tmp = *(rgbn_views[i]);
			pcl::PointCloud<PointXYZSHOT> tmp_shot = *(shot_views[i]);
			pcl::transformPointCloud(tmp, tmp, lum_sift.getTransformation (i));
			pcl::transformPointCloud(tmprgb, tmprgb, lum_sift.getTransformation (i));
			pcl::transformPointCloud(tmp_shot, tmp_shot, lum_sift.getTransformation (i));
			*cloud_merged += tmprgb;
			*cloud_shot_merged += tmp_shot;
			//		*cloud_normal_merged += tmp;

		}

		CLOG(LINFO) << "addViewToModel before uniform sampling: "<<cloud_merged->size();

		cloud_sift_merged = lum_sift.getConcatenatedCloud ();
	}

	CLOG(LINFO) << "addViewToModel before uniform sampling: "<<cloud_merged->size();

/*	pcl::PointCloud<int> sampled_indices;
	pcl::UniformSampling<pcl::PointXYZRGB> uniform_sampling;
	uniform_sampling.setInputCloud (cloud_merged);
	uniform_sampling.setRadiusSearch (0.001);
	uniform_sampling.compute (sampled_indices);
	pcl::copyPointCloud (*cloud_merged, sampled_indices.points, *cloud_merged);*/

	CLOG(LINFO) << "addViewToModel after uniform sampling: "<<cloud_merged->size();

//*cloud_sift_merged += *cloud_sift;
//
	CLOG(LINFO) << "SENDING ... model cloud_merged->size(): "<< cloud_merged->size();
//	CLOG(LINFO) << "SENDING ... model cloud_normal_merged->size(): "<< cloud_normal_merged->size();
	CLOG(LINFO) << "SENDING ... model cloud_sift_merged->size(): "<< cloud_sift_merged->size();
	CLOG(LINFO) << "SENDING ... model cloud_shot_merged->size(): "<< cloud_shot_merged->size();

// Compute mean number of features.
//mean_viewpoint_features_number = total_viewpoint_features_number/counter;

// Push results to output data ports.
	out_mean_viewpoint_features_number.write(total_viewpoint_features_number/counter);
	out_cloud_xyzrgb.write(cloud_merged);
	out_cloud_xyzrgb_normals.write(cloud_normal_merged);
	out_cloud_xyzsift.write(cloud_sift_merged);
	out_cloud_xyzshot.write(cloud_shot_merged);

}

} // namespace ClosedCloudMerge
} // namespace Processors
