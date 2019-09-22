// ------------------------------------------------------------------
// Fast R-CNN
// Copyright (c) 2015 Microsoft
// Licensed under The MIT License [see fast-rcnn/LICENSE for details]
// Written by Ross Girshick
// ------------------------------------------------------------------

// modify by github.com/makefile
#include "fpn_utils.hpp"
#include "fpn_proposal_target_layer.hpp"

namespace caffe {

namespace Frcnn {

using std::vector;

template <typename Dtype>
void FPNProposalTargetLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype> *> &bottom,
                                                 const vector<Blob<Dtype> *> &top) {
  this->rng_.reset(new Caffe::RNG(static_cast<unsigned int>(FrcnnParam::rng_seed)));
  this->_count_ = this->_bg_num_ = this->_fg_num_ = 0;

  config_n_classes_ = FrcnnParam::n_classes;

  LOG(INFO) << "FPNProposalTargetLayer :: " << config_n_classes_ << " classes";
  LOG(INFO) << "FPNProposalTargetLayer :: LayerSetUp";
  // sampled rois (0, x1, y1, x2, y2)
  top[0]->Reshape(1, 5, 1, 1);
  top[1]->Reshape(1, 5, 1, 1);
  top[2]->Reshape(1, 5, 1, 1);
  top[3]->Reshape(1, 5, 1, 1);
  // top[4]->Reshape(1, 5, 1, 1);
  int top_idx = 3;//the following top blob all has at least num=4 same as pyramid level,though there is not sure what the num will be in forward,we have to set this,because Caffe will check num consistence before forward.
  // labels
  top[top_idx + 1]->Reshape(4, 1, 1, 1);
  // bbox_targets
  top[top_idx + 2]->Reshape(4, config_n_classes_ * 4, 1, 1);
  // bbox_inside_weights
  top[top_idx + 3]->Reshape(4, config_n_classes_ * 4, 1, 1);
  // bbox_outside_weights
  top[top_idx + 4]->Reshape(4, config_n_classes_ * 4, 1, 1);
  _forward_iter_ = 0; //fyk add for logging periodly
}

template <typename Dtype>
void FPNProposalTargetLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype> *> &bottom, const vector<Blob<Dtype> *> &top) {

  vector<Point4f<Dtype> > all_rois;
  for (int i = 0; i < bottom[0]->num(); i++) {
    all_rois.push_back(Point4f<Dtype>(
        bottom[0]->data_at(i,1,0,0),
        bottom[0]->data_at(i,2,0,0),
        bottom[0]->data_at(i,3,0,0),
        bottom[0]->data_at(i,4,0,0)));
    CHECK_EQ(bottom[0]->data_at(i,0,0,0), 0) << "Only single item batches are supported";
  }

  vector<Point4f<Dtype> > gt_boxes;
  vector<int> gt_labels;
  for (int i = 0; i < bottom[1]->num(); i++) {
    gt_boxes.push_back(Point4f<Dtype>(
        bottom[1]->data_at(i,0,0,0),
        bottom[1]->data_at(i,1,0,0),
        bottom[1]->data_at(i,2,0,0),
        bottom[1]->data_at(i,3,0,0)));
    gt_labels.push_back(bottom[1]->data_at(i,4,0,0));
    CHECK_GT(gt_labels[i], 0) << "Ground Truth Should Be Greater Than 0";
  }

  all_rois.insert(all_rois.end(), gt_boxes.begin(), gt_boxes.end());

  DLOG(ERROR) << "gt boxes size: " << gt_boxes.size();
  const int num_images = 1;
  int rois_per_image = FrcnnParam::batch_size / num_images;
  int fg_rois_per_image = rois_per_image * FrcnnParam::fg_fraction;
  if (FrcnnParam::batch_size == -1) {
    rois_per_image = all_rois.size();
    fg_rois_per_image = rois_per_image;
  }

  //Sample rois with classification labels and bounding box regression
  //targets
  vector<int> labels;
  vector<Point4f<Dtype> > rois;
  vector<vector<Point4f<Dtype> > > bbox_targets, bbox_inside_weights;

  _sample_rois(all_rois, gt_boxes, gt_labels, fg_rois_per_image, rois_per_image, labels, rois, bbox_targets, bbox_inside_weights);

  CHECK_EQ(labels.size(), rois.size());
  CHECK_EQ(labels.size(), bbox_targets.size());
  CHECK_EQ(labels.size(), bbox_inside_weights.size());
  // FPN: assign to pyramid level
  // split rois to several level, notice that bbox_targets & bbox_inside_weights must be corresponding with rois index
  int n_level = 4; // fpn_levels
  vector<vector<Point4f<Dtype> > > level_rois (n_level, vector<Point4f<Dtype> >());
  vector<vector<int> > level_labels (n_level);
  vector<vector<vector<Point4f<Dtype> > > > level_targets(n_level), level_weights(n_level);
  for (size_t i = 0; i < rois.size(); i++) {
    int level_idx = calc_level(rois[i], n_level + 1) - 2;

    level_rois[level_idx].push_back(rois[i]);
    level_labels[level_idx].push_back(labels[i]);
    level_targets[level_idx].push_back(bbox_targets[i]);
    level_weights[level_idx].push_back(bbox_inside_weights[i]);
  }
  labels.clear();
  bbox_targets.clear();
  bbox_inside_weights.clear();
  Point4f<Dtype> pad_roi;
  Point4f<Dtype> zeros[config_n_classes_];
  vector<Point4f<Dtype> > pad_zeros(zeros, zeros + config_n_classes_);
  for (size_t j = 0; j < n_level; j++) {
    if (level_rois[j].size() == 0) {
      level_rois[j].push_back(pad_roi);
      labels.push_back(-1); // set ignore_label: -1 in latter layer to ignore this roi
      bbox_targets.push_back(pad_zeros);
      bbox_inside_weights.push_back(pad_zeros);
    } else {
      labels.insert(labels.end(), level_labels[j].begin(), level_labels[j].end());
      bbox_targets.insert(bbox_targets.end(), level_targets[j].begin(), level_targets[j].end());
      bbox_inside_weights.insert(bbox_inside_weights.end(), level_weights[j].begin(), level_weights[j].end());
    }
  }
  split_top_rois_by_level(top,0,level_rois);//save leveled rois to top blob
  const int batch_size = labels.size();
  _forward_iter_ ++;
  if (_forward_iter_ >= 200) {
    LOG(INFO) << "FPNProposalTargetLayer => rois num: " << batch_size << " = " << top[0]->num() << " + " << top[1]->num() << " + " << top[2]->num() << " + " << top[3]->num();// << " + " << top[4]->num();
    _forward_iter_ = 0;	
  }
  int top_idx = 3;//the following top blob
  // classification labels
  top[top_idx + 1]->Reshape(batch_size, 1, 1, 1);
  Dtype *label_data = top[top_idx + 1]->mutable_cpu_data();
  for (int i = 0; i < batch_size; i++) {
    label_data[ top[top_idx + 1]->offset(i,0,0,0) ] = labels[i];
  }
  // bbox_targets
  top[top_idx + 2]->Reshape(batch_size, this->config_n_classes_*4, 1, 1);
  caffe_set(top[top_idx + 2]->count(), Dtype(0), top[top_idx + 2]->mutable_cpu_data());
  Dtype *target_data = top[top_idx + 2]->mutable_cpu_data();
  // bbox_inside_weights and bbox_outside_weights
  top[top_idx + 3]->Reshape(batch_size, this->config_n_classes_*4, 1, 1); //bbox_inside_weights
  caffe_set(top[top_idx + 3]->count(), Dtype(0), top[top_idx + 3]->mutable_cpu_data());
  Dtype *bbox_inside_data = top[top_idx + 3]->mutable_cpu_data();
  top[top_idx + 4]->Reshape(batch_size, this->config_n_classes_*4, 1, 1); //bbox_outside_weights
  caffe_set(top[top_idx + 4]->count(), Dtype(0), top[top_idx + 4]->mutable_cpu_data());
  Dtype *bbox_outside_data = top[top_idx + 4]->mutable_cpu_data();
  for (int i = 0; i < batch_size; i++) {
    for (int j = 0; j < this->config_n_classes_; j++) {
      for (int cor = 0; cor < 4; cor++ ) {
        target_data[ top[top_idx + 2]->offset(i, j*4+cor, 0, 0) ] = bbox_targets[i][j][cor];
        bbox_inside_data[ top[top_idx + 2]->offset(i, j*4+cor, 0, 0) ] = bbox_inside_weights[i][j][cor];
        bbox_outside_data[ top[top_idx + 2]->offset(i, j*4+cor, 0, 0) ] = bbox_inside_weights[i][j][cor] > 0;
      }
    }
  }
  DLOG(INFO) << "FPNProposalTargetLayer::Forward_cpu End";
}

template <typename Dtype>
void FPNProposalTargetLayer<Dtype>::Backward_cpu(
    const vector<Blob<Dtype> *> &top, const vector<bool> &propagate_down,
    const vector<Blob<Dtype> *> &bottom) {
  for (int i = 0; i < propagate_down.size(); ++i) {
    if (propagate_down[i]) {
      NOT_IMPLEMENTED;
    }
  }
}

template <typename Dtype>
void FPNProposalTargetLayer<Dtype>::_sample_rois(const vector<Point4f<Dtype> > &all_rois, const vector<Point4f<Dtype> > &gt_boxes, 
        const vector<int> &gt_label, const int fg_rois_per_image, const int rois_per_image, vector<int> &labels, 
        vector<Point4f<Dtype> > &rois, vector<vector<Point4f<Dtype> > > &bbox_targets, vector<vector<Point4f<Dtype> > > &bbox_inside_weights) {
// Generate a random sample of RoIs comprising foreground and background examples.

  CHECK_EQ(gt_label.size(), gt_boxes.size());
  // overlaps: (rois x gt_boxes)
  std::vector<std::vector<Dtype> > overlaps = get_ious(all_rois, gt_boxes, this->use_gpu_nms_in_forward_cpu);
  this->use_gpu_nms_in_forward_cpu = false; // restore
  std::vector<Dtype> max_overlaps(all_rois.size(), 0);
  std::vector<int> gt_assignment(all_rois.size(), -1);
  std::vector<int> _labels(all_rois.size());
  for (int i = 0; i < all_rois.size(); ++ i) {
    for (int j = 0; j < gt_boxes.size(); ++ j) {
      if (max_overlaps[i] <= overlaps[i][j]) {
        max_overlaps[i] = overlaps[i][j];
        gt_assignment[i] = j;       
      }
    }
  }
  DLOG(INFO) << "sample_rois : all_rois: " << all_rois.size() << ", gt_box: " << gt_boxes.size();
  for (size_t i = 0; i < all_rois.size(); ++i) {
    if (gt_assignment[i] >= 0 ) {
      CHECK_LT(gt_assignment[i], gt_label.size());
      _labels[i] = gt_label[gt_assignment[i]];
    } else {
      _labels[i] = 0;
    }
  }
  
  // Select foreground RoIs as those with >= FG_THRESH overlap
  std::vector<int> fg_inds;
  for (int i = 0; i < all_rois.size(); ++i) {
    if (max_overlaps[i] >= FrcnnParam::fg_thresh) {
      fg_inds.push_back(i);
    }
  }
  // Guard against the case when an image has fewer than fg_rois_per_image
  // foreground RoIs
  const int fg_rois_per_this_image = std::min(fg_rois_per_image, int(fg_inds.size()));
  DLOG(INFO) << "fg_inds [PRE,AFT] : [" << fg_inds.size() << "," << fg_rois_per_this_image << "] FG_THRESH : " << FrcnnParam::fg_thresh;
  // Sample foreground regions without replacement
  if (fg_inds.size() > 0) {
    shuffle(fg_inds.begin(), fg_inds.end(), (caffe::rng_t *) this->rng_->generator());
    fg_inds.resize(fg_rois_per_this_image);
  }
  
  // Select background RoIs as those within [BG_THRESH_LO, BG_THRESH_HI)
  std::vector<int> bg_inds;
  for (int i = 0; i < all_rois.size(); ++i) {
    if (max_overlaps[i] >= FrcnnParam::bg_thresh_lo 
        && max_overlaps[i] < FrcnnParam::bg_thresh_hi) {
      bg_inds.push_back(i);
    }
  }
  // Compute number of background RoIs to take from this image (guarding against there being fewer than desired)
  const int bg_rois_per_this_image = std::min(rois_per_image-fg_rois_per_this_image, int(bg_inds.size()));
  DLOG(INFO) << "bg_inds [PRE,AFT] : [" << bg_inds.size() << "," << bg_rois_per_this_image << "] BG_THRESH : [" << FrcnnParam::bg_thresh_lo << ", " << FrcnnParam::bg_thresh_hi << ")" ;
  // Sample background regions without replacement
  if (bg_inds.size() > 0) {
    shuffle(bg_inds.begin(), bg_inds.end(), (caffe::rng_t *) this->rng_->generator());
    bg_inds.resize(bg_rois_per_this_image);
  }

  // The indices that we're selecting (both fg and bg)
  std::vector<int> keep_inds(fg_inds);
  keep_inds.insert(keep_inds.end(), bg_inds.begin(), bg_inds.end());

  // fyk add for solving the problem of zero roi problem, better than setting bg_thresh_lo to 0 in config
  // Need more?
  int remaining = rois_per_image - keep_inds.size();
  if (remaining > 0) {
    // Looks like we don't have enough samples to maintain the desired
    // balance. Reduce requirements and fill in the rest. This is
    // likely different from the Mask RCNN paper.
    // There is a small chance we have neither fg nor bg samples.
    if (keep_inds.size() == 0) {
        // Pick bg regions with easier IoU threshold
        for (int i = 0; i < all_rois.size(); ++i) {
          if (max_overlaps[i] < FrcnnParam::bg_thresh_lo) { 
            bg_inds.push_back(i);
          }
        }
        if (bg_inds.size() > 0) {
            shuffle(bg_inds.begin(), bg_inds.end(), (caffe::rng_t *) this->rng_->generator());
            bg_inds.resize(rois_per_image);
            keep_inds.insert(keep_inds.end(), bg_inds.begin(), bg_inds.end());
        }
    }else{
	if (bg_inds.size() == 0) {
            for (int i = 0; i < all_rois.size(); ++i) {
	      // Negative ROIs are those with max IoU 0.1-0.5 (hard example mining)
	      // To hard example mine or not to hard example mine, that's the question
              if (max_overlaps[i] < FrcnnParam::bg_thresh_hi) {
                bg_inds.push_back(i);
              }
            }
	}
        // Fill the rest with repeated bg rois.
        if (bg_inds.size() > 0) while (remaining > 0) {
            int rmin = std::min(remaining, (int)bg_inds.size());
            keep_inds.insert(keep_inds.end(), bg_inds.begin(), bg_inds.begin() + rmin);
            remaining -= rmin;
        }
    }
  }

  // Select sampled values from various arrays:
  labels.resize(keep_inds.size());
  rois.resize(keep_inds.size());
  std::vector<Point4f<Dtype> > _gt_boxes(keep_inds.size());
  for (size_t i = 0; i < keep_inds.size(); ++ i) {
    labels[i] = _labels[keep_inds[i]];
    rois[i] = all_rois[keep_inds[i]];
    _gt_boxes[i] =
        gt_assignment[keep_inds[i]] >= 0 ? gt_boxes[gt_assignment[keep_inds[i]]] : Point4f<Dtype>();
    // Clamp labels for the background RoIs to 0
    if ( i >= fg_rois_per_this_image ) 
        labels[i] = 0;
  }

#ifdef DEBUG
  DLOG(INFO) << "num fg : " << labels.size() - std::count(labels.begin(), labels.end(), 0);
  DLOG(INFO) << "num bg : " << std::count(labels.begin(), labels.end(), 0);
  CHECK_EQ(std::count(labels.begin(), labels.end(), -1), 0);
  this->_count_ += 1;
  this->_fg_num_ += labels.size() - std::count(labels.begin(), labels.end(), 0);
  this->_bg_num_ += std::count(labels.begin(), labels.end(), 0);
  DLOG(INFO) << "num fg avg : " << this->_fg_num_ * 1. / this->_count_;
  DLOG(INFO) << "num bg avg : " << this->_bg_num_ * 1. / this->_count_;
  DLOG(INFO) << "ratio : " << float(this->_fg_num_) / float(this->_bg_num_+FrcnnParam::eps);
  DLOG(INFO) << "FrcnnParam::bbox_normalize_targets : " << (FrcnnParam::bbox_normalize_targets ? "True" : "False");
  DLOG(INFO) << "FrcnnParam::bbox_normalize_means : " << FrcnnParam::bbox_normalize_means[0] << ", " << FrcnnParam::bbox_normalize_means[1]
        << ", " << FrcnnParam::bbox_normalize_means[2] << ", " << FrcnnParam::bbox_normalize_means[3];
  DLOG(INFO) << "FrcnnParam::bbox_normalize_stds : " << FrcnnParam::bbox_normalize_stds[0] << ", " << FrcnnParam::bbox_normalize_stds[1]
        << ", " << FrcnnParam::bbox_normalize_stds[2] << ", " << FrcnnParam::bbox_normalize_stds[3];
#endif

  //def _compute_targets(ex_rois, gt_rois, labels):
  CHECK_EQ(rois.size(), _gt_boxes.size());
  CHECK_EQ(rois.size(), labels.size());
  std::vector<Point4f<Dtype> > bbox_targets_data = bbox_transform(rois, _gt_boxes);
  if ( FrcnnParam::bbox_normalize_targets ) {
    // Optionally normalize targets by a precomputed mean and stdev
    for (size_t index = 0; index < bbox_targets_data.size(); index ++) {
      for (int j = 0; j < 4; j ++) {
        bbox_targets_data[index][j] = (bbox_targets_data[index][j]-FrcnnParam::bbox_normalize_means[j]) / FrcnnParam::bbox_normalize_stds[j];
      }
    }
  }

  // Compute boxes target 
  bbox_targets = std::vector<std::vector<Point4f<Dtype> > >(
          keep_inds.size(), std::vector<Point4f<Dtype> >(this->config_n_classes_));
  bbox_inside_weights = std::vector<std::vector<Point4f<Dtype> > >(
          keep_inds.size(), std::vector<Point4f<Dtype> >(this->config_n_classes_));
  for (size_t i = 0; i < labels.size(); ++i) if (labels[i] > 0) {
    int cls = labels[i];
    //get bbox_targets and bbox_inside_weights
    bbox_targets[i][cls] = bbox_targets_data[i];
    bbox_inside_weights[i][cls] = Point4f<Dtype>(FrcnnParam::bbox_inside_weights);
  }

}

#ifdef CPU_ONLY
STUB_GPU(FPNProposalTargetLayer);
#endif

INSTANTIATE_CLASS(FPNProposalTargetLayer);
//REGISTER_LAYER_CLASS(FPNProposalTarget);
EXPORT_LAYER_MODULE_CLASS(FPNProposalTarget);

} // namespace frcnn

} // namespace caffe
