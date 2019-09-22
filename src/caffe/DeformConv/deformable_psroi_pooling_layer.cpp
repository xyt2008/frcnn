// ------------------------------------------------------------------
// Deformable R-FCN
// Written by Bharat Singh, 2017.
// modify for CPU code by @makefile, 2018.
// ------------------------------------------------------------------

#include <cfloat>
#include <algorithm>

#include <string>
#include <utility>
#include <vector>

#include "deformable_psroi_pooling_layer.hpp"
#include "caffe/util/math_functions.hpp"

using std::max;
using std::min;
using std::floor;
using std::ceil;

namespace caffe {
  template <typename Dtype>
  void DeformablePSROIPoolingLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
    DeformablePSROIPoolingParameter deformable_psroi_pooling_param =
      this->layer_param_.deformable_psroi_pooling_param();
    spatial_scale_ = deformable_psroi_pooling_param.spatial_scale();
    LOG(INFO) << "Spatial scale: " << spatial_scale_;

    CHECK_GT(deformable_psroi_pooling_param.output_dim(), 0)
      << "output_dim must be > 0";
    CHECK_GT(deformable_psroi_pooling_param.group_size(), 0)
      << "group_size must be > 0";
    
    output_dim_ = deformable_psroi_pooling_param.output_dim();
    group_size_ = deformable_psroi_pooling_param.group_size();
    part_size_ = deformable_psroi_pooling_param.part_size();
    sample_per_part_ = deformable_psroi_pooling_param.sample_per_part();
    trans_std_ = deformable_psroi_pooling_param.trans_std();
    no_trans_ = deformable_psroi_pooling_param.no_trans();
    pooled_height_ = group_size_;
    pooled_width_ = group_size_;
  }

  template <typename Dtype>
  void DeformablePSROIPoolingLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
    channels_ = bottom[0]->channels();
    CHECK_EQ(channels_, output_dim_*group_size_*group_size_)
      << "input channel number does not match layer parameters";
    height_ = bottom[0]->height();
    width_ = bottom[0]->width();
    top[0]->Reshape(
      bottom[1]->num(), output_dim_, pooled_height_, pooled_width_);
    mapping_channel_.Reshape(
      bottom[1]->num(), output_dim_, pooled_height_, pooled_width_);
  }

    template <typename DType>
    DType bilinear_interp(
      const DType* data,
      const DType x,
      const DType y,
      const int width,
      const int height) {
      int x1 = floor(x);
      int x2 = ceil(x);
      int y1 = floor(y);
      int y2 = ceil(y);
      DType dist_x = static_cast<DType>(x - x1);
      DType dist_y = static_cast<DType>(y - y1);
      DType value11 = data[y1*width + x1];
      DType value12 = data[y2*width + x1];
      DType value21 = data[y1*width + x2];
      DType value22 = data[y2*width + x2];
      DType value = (1 - dist_x)*(1 - dist_y)*value11 + (1 - dist_x)*dist_y*value12
        + dist_x*(1 - dist_y)*value21 + dist_x*dist_y*value22;
      return value;
    }

    template <typename DType>
    void DeformablePSROIPoolForward(
      const int count,
      const DType* bottom_data,
      const DType spatial_scale,
      const int channels,
      const int height, const int width,
      const int pooled_height, const int pooled_width,
      const DType* bottom_rois, const DType* bottom_trans,
      const bool no_trans,
      const DType trans_std,
      const int sample_per_part,
      const int output_dim,
      const int group_size,
      const int part_size,
      const int num_classes,
      const int channels_each_class,
      DType* top_data,
      DType* top_count) {
      //CUDA_KERNEL_LOOP(index, count) {
      for (int index = 0; index < count; index++) {
        // The output is in order (n, ctop, ph, pw)
        int pw = index % pooled_width;
        int ph = (index / pooled_width) % pooled_height;
        int ctop = (index / pooled_width / pooled_height) % output_dim;
        int n = index / pooled_width / pooled_height / output_dim;

        // [start, end) interval for spatial sampling
        const DType* offset_bottom_rois = bottom_rois + n * 5;
        int roi_batch_ind = offset_bottom_rois[0];
        DType roi_start_w = static_cast<DType>(round(offset_bottom_rois[1])) * spatial_scale - 0.5;
        DType roi_start_h = static_cast<DType>(round(offset_bottom_rois[2])) * spatial_scale - 0.5;
        DType roi_end_w = static_cast<DType>(round(offset_bottom_rois[3]) + 1.) * spatial_scale - 0.5;
        DType roi_end_h = static_cast<DType>(round(offset_bottom_rois[4]) + 1.) * spatial_scale - 0.5;

        // Force too small ROIs to be 1x1
        DType roi_width = max(roi_end_w - roi_start_w, static_cast<DType>(0.1)); //avoid 0
        DType roi_height = max(roi_end_h - roi_start_h, static_cast<DType>(0.1));

        // Compute w and h at bottom
        DType bin_size_h = roi_height / static_cast<DType>(pooled_height);
        DType bin_size_w = roi_width / static_cast<DType>(pooled_width);

        DType sub_bin_size_h = bin_size_h / static_cast<DType>(sample_per_part);
        DType sub_bin_size_w = bin_size_w / static_cast<DType>(sample_per_part);

        int part_h = floor(static_cast<DType>(ph) / pooled_height*part_size);
        int part_w = floor(static_cast<DType>(pw) / pooled_width*part_size);
        int class_id = ctop / channels_each_class;
        DType trans_x = no_trans ? static_cast<DType>(0) :
          bottom_trans[(((n * num_classes + class_id) * 2) * part_size + part_h)*part_size + part_w] * trans_std;
        DType trans_y = no_trans ? static_cast<DType>(0) :
          bottom_trans[(((n * num_classes + class_id) * 2 + 1) * part_size + part_h)*part_size + part_w] * trans_std;
        
        DType wstart = static_cast<DType>(pw)* bin_size_w
          + roi_start_w;
        wstart += trans_x * roi_width;
        DType hstart = static_cast<DType>(ph) * bin_size_h
          + roi_start_h;
        hstart += trans_y * roi_height;
        
        DType sum = 0;
        int count = 0;
        int gw = floor(static_cast<DType>(pw) * group_size / pooled_width);
        int gh = floor(static_cast<DType>(ph)* group_size / pooled_height);
        gw = min(max(gw, 0), group_size - 1);
        gh = min(max(gh, 0), group_size - 1);

        const DType* offset_bottom_data = bottom_data + (roi_batch_ind * channels) * height * width;
        for (int ih = 0; ih < sample_per_part; ih++) {
          for (int iw = 0; iw < sample_per_part; iw++) {
            DType w = wstart + iw*sub_bin_size_w;
            DType h = hstart + ih*sub_bin_size_h;
            // bilinear interpolation
            if (w<-0.5 || w>width - 0.5 || h<-0.5 || h>height - 0.5) {
              continue;
            }
            w = min(max(w, static_cast<DType>(0.)), static_cast<DType>(width - 1.));
            h = min(max(h, static_cast<DType>(0.)), static_cast<DType>(height - 1.));
            int c = (ctop*group_size + gh)*group_size + gw;
            DType val = bilinear_interp(offset_bottom_data + c*height*width, w, h, width, height);
            sum += val;
            count++;
          }
        }
        top_data[index] = count == 0 ? static_cast<DType>(0) : sum / count;
        top_count[index] = count;
      }
    }

  template <typename Dtype>
  void DeformablePSROIPoolingLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
    const Dtype* bottom_data = bottom[0]->cpu_data();
    const Dtype* bottom_rois = bottom[1]->cpu_data();
    const Dtype *bottom_trans = no_trans_ ? NULL : bottom[2]->cpu_data();
    Dtype* top_data = top[0]->mutable_cpu_data();
    Dtype* mapping_channel_ptr = mapping_channel_.mutable_cpu_data();
    int count = top[0]->count();
    const int num_classes = no_trans_ ? 1 : bottom[2]->channels()/ 2;
    const int channels_each_class = no_trans_ ? output_dim_ : output_dim_ / num_classes;
    caffe_set(count, Dtype(0), top_data);
    caffe_set(count, Dtype(0), mapping_channel_ptr);
    // LOG(INFO) << "DeformablePSROIPoolingLayer forward CPU.";
    // NOLINT_NEXT_LINE(whitespace/operators)
    DeformablePSROIPoolForward<Dtype> (
        count, bottom_data, spatial_scale_, channels_, height_, width_, pooled_height_, pooled_width_,
        bottom_rois, bottom_trans, no_trans_, trans_std_, sample_per_part_, output_dim_, 
        group_size_, part_size_, num_classes, channels_each_class, top_data, mapping_channel_ptr);
  }

#ifdef CPU_ONLY
  STUB_GPU(DeformablePSROIPoolingLayer);
#endif

  INSTANTIATE_CLASS(DeformablePSROIPoolingLayer);
  REGISTER_LAYER_CLASS(DeformablePSROIPooling);

}  // namespace caffe