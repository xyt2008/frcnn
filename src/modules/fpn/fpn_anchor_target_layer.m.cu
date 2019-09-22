// ------------------------------------------------------------------
// Fast R-CNN
// Copyright (c) 2015 Microsoft
// Licensed under The MIT License [see fast-rcnn/LICENSE for details]
// Written by Ross Girshick
// ------------------------------------------------------------------

#include "fpn_anchor_target_layer.m.hpp"
#include "caffe/FRCNN/util/frcnn_helper.hpp"
#include "caffe/FRCNN/util/frcnn_utils.hpp"
#include "caffe/FRCNN/util/frcnn_param.hpp"

namespace caffe {

namespace Frcnn {

template <typename Dtype>
void FPNAnchorTargetLayer<Dtype>::Forward_gpu(
  const vector<Blob<Dtype> *> &bottom, const vector<Blob<Dtype> *> &top) {
  this->use_gpu_nms_in_forward_cpu = true; // set flag to be used in forward_cpu
  this->Forward_cpu(bottom, top);
}

template <typename Dtype>
void FPNAnchorTargetLayer<Dtype>::Backward_gpu(
    const vector<Blob<Dtype> *> &top, const vector<bool> &propagate_down,
    const vector<Blob<Dtype> *> &bottom) {
  for (int i = 0; i < propagate_down.size(); ++i) {
    if (propagate_down[i]) {
      NOT_IMPLEMENTED;
    }
  }
}

INSTANTIATE_LAYER_GPU_FUNCS(FPNAnchorTargetLayer);

} // namespace frcnn

} // namespace caffe
