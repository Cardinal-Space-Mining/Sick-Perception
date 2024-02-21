#pragma once

#include <vector>
#include <limits>

#include <pcl/types.h>
#include <pcl/common/io.h>
#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/filters/impl/voxel_grid.hpp>		// includes <pcl/common/centroid.h> and <boost/sort/spreadsort/integer_sort.hpp> which we use
#include <pcl/filters/morphological_filter.h>


/** Copied from VoxelGrid<>::applyFilter() and removed unnecessary additions */
template<
	typename PointT = pcl::PointXYZ,
	typename IntT = pcl::index_t,
	typename FloatT = float>
void voxel_filter(
	const pcl::PointCloud<PointT>& cloud,
	const std::vector<IntT>& selection,
	pcl::PointCloud<PointT>& voxelized,
	FloatT leaf_x, FloatT leaf_y, FloatT leaf_z,
	unsigned int min_points_per_voxel_ = 0,
	bool downsample_all_data_ = false
) {
	const bool use_selection = !selection.empty();

	const Eigen::Vector4f
		leaf_size_{ leaf_x, leaf_y, leaf_z, 1.f };
	const Eigen::Array4f
		inverse_leaf_size_{ Eigen::Array4f::Ones() / leaf_size_.array() };

	// Copy the header (and thus the frame_id) + allocate enough space for points
	voxelized.height       = 1;                    // downsampling breaks the organized structure
	voxelized.is_dense     = true;                 // we filter out invalid points

	Eigen::Vector4f min_p, max_p;
	// Get the minimum and maximum dimensions
	if(use_selection) {
		pcl::getMinMax3D<PointT>(cloud, selection, min_p, max_p);
	} else {
		pcl::getMinMax3D<PointT>(cloud, min_p, max_p);
	}

	// Check that the leaf size is not too small, given the size of the data
	std::int64_t dx = static_cast<std::int64_t>((max_p[0] - min_p[0]) * inverse_leaf_size_[0]) + 1;
	std::int64_t dy = static_cast<std::int64_t>((max_p[1] - min_p[1]) * inverse_leaf_size_[1]) + 1;
	std::int64_t dz = static_cast<std::int64_t>((max_p[2] - min_p[2]) * inverse_leaf_size_[2]) + 1;

	if( (dx * dy * dz) > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) ) {
		voxelized.clear();
		return;
	}

	Eigen::Vector4i min_b_, max_b_, div_b_, divb_mul_;

	// Compute the minimum and maximum bounding box values
	min_b_[0] = static_cast<int> ( std::floor(min_p[0] * inverse_leaf_size_[0]) );
	max_b_[0] = static_cast<int> ( std::floor(max_p[0] * inverse_leaf_size_[0]) );
	min_b_[1] = static_cast<int> ( std::floor(min_p[1] * inverse_leaf_size_[1]) );
	max_b_[1] = static_cast<int> ( std::floor(max_p[1] * inverse_leaf_size_[1]) );
	min_b_[2] = static_cast<int> ( std::floor(min_p[2] * inverse_leaf_size_[2]) );
	max_b_[2] = static_cast<int> ( std::floor(max_p[2] * inverse_leaf_size_[2]) );

	// Compute the number of divisions needed along all axis
	div_b_ = max_b_ - min_b_ + Eigen::Vector4i::Ones ();
	div_b_[3] = 0;

	// Set up the division multiplier
	divb_mul_ = Eigen::Vector4i{ 1, div_b_[0], div_b_[0] * div_b_[1], 0 };

	// Storage for mapping leaf and pointcloud indexes
	std::vector<cloud_point_index_idx> index_vector;

	// First pass: go over all points and insert them into the index_vector vector
	// with calculated idx. Points with the same idx value will contribute to the
	// same point of resulting CloudPoint
	if(use_selection) {
		index_vector.reserve(selection.size());
		for(const auto& index : selection) {
			if(!cloud.is_dense && !pcl::isXYZFinite(cloud[index])) continue;

			int ijk0 = static_cast<int>( std::floor(cloud[index].x * inverse_leaf_size_[0]) - static_cast<float>(min_b_[0]) );
			int ijk1 = static_cast<int>( std::floor(cloud[index].y * inverse_leaf_size_[1]) - static_cast<float>(min_b_[1]) );
			int ijk2 = static_cast<int>( std::floor(cloud[index].z * inverse_leaf_size_[2]) - static_cast<float>(min_b_[2]) );

			// Compute the centroid leaf index
			int idx = ijk0 * divb_mul_[0] + ijk1 * divb_mul_[1] + ijk2 * divb_mul_[2];
			index_vector.emplace_back( static_cast<unsigned int>(idx), index );
		}
	} else {
		index_vector.reserve(cloud.size());
		for(IntT index = 0; index < cloud.size(); index++) {
			if(!cloud.is_dense && !pcl::isXYZFinite(cloud[index])) continue;

			int ijk0 = static_cast<int>( std::floor(cloud[index].x * inverse_leaf_size_[0]) - static_cast<float>(min_b_[0]) );
			int ijk1 = static_cast<int>( std::floor(cloud[index].y * inverse_leaf_size_[1]) - static_cast<float>(min_b_[1]) );
			int ijk2 = static_cast<int>( std::floor(cloud[index].z * inverse_leaf_size_[2]) - static_cast<float>(min_b_[2]) );

			// Compute the centroid leaf index
			int idx = ijk0 * divb_mul_[0] + ijk1 * divb_mul_[1] + ijk2 * divb_mul_[2];
			index_vector.emplace_back( static_cast<unsigned int>(idx), index );
		}
	}

	// Second pass: sort the index_vector vector using value representing target cell as index
	// in effect all points belonging to the same output cell will be next to each other
	auto rightshift_func = [](const cloud_point_index_idx &x, const unsigned offset) { return x.idx >> offset; };
	boost::sort::spreadsort::integer_sort(index_vector.begin(), index_vector.end(), rightshift_func);

	// Third pass: count output cells
	// we need to skip all the same, adjacent idx values
	unsigned int total = 0;
	unsigned int index = 0;
	// first_and_last_indices_vector[i] represents the index in index_vector of the first point in
	// index_vector belonging to the voxel which corresponds to the i-th output point,
	// and of the first point not belonging to.
	std::vector<std::pair<unsigned int, unsigned int> > first_and_last_indices_vector;
	// Worst case size
	first_and_last_indices_vector.reserve (index_vector.size());
	while(index < index_vector.size()) {
		unsigned int i = index + 1;
		for(; i < index_vector.size() && index_vector[i].idx == index_vector[index].idx; ++i);
		if (i - index >= min_points_per_voxel_) {
			++total;
			first_and_last_indices_vector.emplace_back(index, i);
		}
		index = i;
	}

	// Fourth pass: compute centroids, insert them into their final position
	voxelized.resize(total);

	index = 0;
	for (const auto &cp : first_and_last_indices_vector) {
		// calculate centroid - sum values from all input points, that have the same idx value in index_vector array
		unsigned int first_index = cp.first;
		unsigned int last_index = cp.second;

		//Limit downsampling to coords
		if (!downsample_all_data_) {
			Eigen::Vector4f centroid{ Eigen::Vector4f::Zero() };

			for (unsigned int li = first_index; li < last_index; ++li) {
				centroid += cloud[index_vector[li].cloud_point_index].getVector4fMap();
			}
			centroid /= static_cast<float> (last_index - first_index);
			voxelized[index].getVector4fMap() = centroid;
		}
		else {
			pcl::CentroidPoint<PointT> centroid;

			// fill in the accumulator with leaf points
			for (unsigned int li = first_index; li < last_index; ++li) {
				centroid.add( cloud[index_vector[li].cloud_point_index] );
			}
			centroid.get(voxelized[index]);
		}
		++index;
	}
	voxelized.width = voxelized.size ();

}

/** PMF Filter Reimpl -- See <pcl/filters/progressive_morphological_filter.h> */
template<
	typename PointT = pcl::PointXYZ,
	typename IntT = pcl::index_t>
void progressive_morph_filter(
	const pcl::PointCloud<PointT>& cloud_,
	const std::vector<IntT>& selection,
	std::vector<IntT>& ground,
	const float base_,
	const int max_window_size_,
	const float cell_size_,
	const float initial_distance_,
	const float max_distance_,
	const float slope_,
	const bool exponential_
) {
	// Compute the series of window sizes and height thresholds
	std::vector<float> height_thresholds;
	std::vector<float> window_sizes;
	int iteration = 0;
	float window_size = 0.0f;
	float height_threshold = 0.0f;

	while (window_size < max_window_size_)
	{
		// Determine the initial window size.
		if (exponential_)
			window_size = cell_size_ * (2.0f * std::pow(base_, iteration) + 1.0f);
		else
			window_size = cell_size_ * (2.0f * (iteration + 1) * base_ + 1.0f);

		// Calculate the height threshold to be used in the next iteration.
		if (iteration == 0)
			height_threshold = initial_distance_;
		else
			height_threshold = slope_ * (window_size - window_sizes[iteration - 1]) * cell_size_ + initial_distance_;

		// Enforce max distance on height threshold
		if (height_threshold > max_distance_)
			height_threshold = max_distance_;

		window_sizes.push_back(window_size);
		height_thresholds.push_back(height_threshold);

		iteration++;
	}

	// Ground indices are initially limited to those points in the input cloud we
	// wish to process
	if (selection.size() > 0 && selection.size() <= cloud_.size()) {
		ground = selection;
	} else {
		ground.resize(cloud_.size());
		for (std::size_t i = 0; i < cloud_.size(); i++) {
			ground[i] = i;
		}
	}

	// Progressively filter ground returns using morphological open
	for (std::size_t i = 0; i < window_sizes.size(); ++i)
	{
		// Limit filtering to those points currently considered ground returns
		typename pcl::PointCloud<PointT>::Ptr cloud(new pcl::PointCloud<PointT>);
		pcl::copyPointCloud<PointT>(cloud_, ground, *cloud);

		// Create new cloud to hold the filtered results. Apply the morphological
		// opening operation at the current window size.
		typename pcl::PointCloud<PointT>::Ptr
			cloud_O(new pcl::PointCloud<PointT>),
			cloud_C(new pcl::PointCloud<PointT>);
		pcl::applyMorphologicalOperator<PointT>(cloud, window_sizes[i], pcl::MorphologicalOperators::MORPH_OPEN, *cloud_O);
		pcl::applyMorphologicalOperator<PointT>(cloud, window_sizes[i], pcl::MorphologicalOperators::MORPH_CLOSE, *cloud_C);

		// Find indices of the points whose difference between the source and
		// filtered point clouds is less than the current height threshold.
		pcl::Indices pt_indices;
		for (std::size_t p_idx = 0; p_idx < ground.size(); ++p_idx)
		{
			float diff_O = (*cloud)[p_idx].z - (*cloud_O)[p_idx].z;
			float diff_C = (*cloud_C)[p_idx].z - (*cloud)[p_idx].z;
			if (diff_O < height_thresholds[i] && diff_C < height_thresholds[i])
				pt_indices.push_back(ground[p_idx]);
		}

		// Ground is now limited to pt_indices
		ground.swap(pt_indices);
	}

}

/** Generate a set of ranges for each point in the provided cloud */
template<
	typename PointT = pcl::PointXYZ,
	typename AllocT = typename pcl::PointCloud<PointT>::VectorType::allocator_type,
	typename IntT = pcl::index_t,
	typename FloatT = float>
void pc_generate_ranges(
	const std::vector<PointT, AllocT>& points,
	const std::vector<IntT>& selection,
	std::vector<FloatT>& out_ranges,
	const Eigen::Vector3<FloatT>& origin
) {
	if (!selection.empty()) {
		out_ranges.resize(selection.size());
		for (int i = 0; i < selection.size(); i++) {
			out_ranges[i] = (origin - *reinterpret_cast<const Eigen::Vector3<FloatT>*>(&points[selection[i]])).norm();
		}
	}
	else {
		out_ranges.resize(points.size());
		for (int i = 0; i < points.size(); i++) {
			out_ranges[i] = (origin - *reinterpret_cast<const Eigen::Vector3<FloatT>*>(&points[i])).norm();
		}
	}
}
/** Remove the points at the each index in the provided set */
template<
	typename PointT = pcl::PointXYZ,
	typename AllocT = typename pcl::PointCloud<PointT>::VectorType::allocator_type,
	typename IntT = pcl::index_t>
void pc_remove_selection(
	std::vector<PointT, AllocT>& points,
	const std::vector<IntT>& selection
) {
	// assert sizes
	size_t last = points.size() - 1;
	for(size_t i = 0; i < selection.size(); i++) {
		memcpy(&points[selection[i]], &points[last], sizeof(PointT));
		last--;
	}
	points.resize(last + 1);
}

/** Filter a set of ranges to an inclusive set of indices */
template<
	typename FloatT = float,
	typename IntT = pcl::index_t>
void pc_filter_ranges(
	const std::vector<FloatT>& ranges,
	const std::vector<IntT>& selection,
	std::vector<IntT>& filtered,
	const FloatT min, const FloatT max
) {
	filtered.clear();
	if(!selection.empty()) {
		filtered.reserve(selection.size());
		for (size_t i = 0; i < selection.size(); i++) {
			const IntT idx = selection[i];
			const FloatT r = ranges[idx];
			if (r <= max && r >= min) {
				filtered.push_back(idx);
			}

		}
	} else {
		filtered.reserve(ranges.size());
		for (size_t i = 0; i < ranges.size(); i++) {
			const FloatT r = ranges[i];
			if (r <= max && r >= min) {
				filtered.push_back(i);
			}
		}
	}
}

/** Given a base set of indices A and a subset of indices B, get (A - B).
 * prereq: selection indices must be in ascending order */
template<typename IntT = pcl::index_t>
void pc_negate_selection(
	const std::vector<IntT>& base,
	const std::vector<IntT>& selection,
	std::vector<IntT>& negated
) {
	if(base.size() <= selection.size()) {
		return;
	}
	negated.resize(base.size() - selection.size());
	size_t
		_base = 0,
		_select = 0,
		_negate = 0;
	for(; _base < base.size() && _negate < negated.size(); _base++) {
		if(_select < selection.size() && base[_base] == selection[_select]) {
			_select++;
		} else {
			negated[_negate] = base[_base];
			_negate++;
		}
	}
}
/** Given a base set of indices A and a subset of indices B, get (A - B).
 * prereq: selection indices must be in ascending order */
template<typename IntT = pcl::index_t>
void pc_negate_selection(
	const IntT base_range,
	const std::vector<IntT>& selection,
	std::vector<IntT>& negated
) {
	if (base_range <= selection.size()) {
		return;
	}
	negated.resize(base_range - selection.size());
	size_t
		_base = 0,
		_select = 0,
		_negate = 0;
	for (; _base < base_range && _negate < negated.size(); _base++) {
		if (_select < selection.size() && _base == selection[_select]) {
			_select++;
		} else /*if (_base < selection[_select])*/ {
			negated[_negate] = _base;
			_negate++;
		}
	}
}
