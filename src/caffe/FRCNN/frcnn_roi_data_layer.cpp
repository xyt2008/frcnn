#include <opencv2/highgui/highgui_c.h>
#include <stdint.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/opencv.hpp>
//#include "opencv2/core/version.hpp"
//#if CV_MAJOR_VERSION == 2
//#include "opencv2/core/core.hpp"
//#include "opencv2/highgui/highgui.hpp"
//#include "opencv2/imgproc/imgproc.hpp"
//
//#elif CV_MAJOR_VERSION == 3
//#include "opencv2/core.hpp"
//#include "opencv2/highgui.hpp"
//#include "opencv2/imgproc.hpp"
//
//#endif
//fyk
#include "caffe/SSD/util/im_transforms.hpp"
#include "data_enhance/histgram/equalize_hist.hpp"
#include "data_augment/data_utils.hpp"
#include "data_enhance/haze_free/haze.h"

#include "caffe/data_transformer.hpp"
#include "caffe/internal_thread.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/util/benchmark.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

#include "caffe/FRCNN/frcnn_roi_data_layer.hpp"
#include "caffe/FRCNN/util/frcnn_utils.hpp"
#include "caffe/FRCNN/util/frcnn_param.hpp"

//#define DEBUG_OUTPUT

// caffe.proto > LayerParameter > FrcnnRoiDataLayer
//   'source' field specifies the window_file
//   'crop_size' indicates the desired warped size
// label start from 1
// x1 y1 start from 1 , in the input file , so we -1 for every corrdinate

namespace caffe {

namespace Frcnn {

template <typename Dtype>
 FrcnnRoiDataLayer<Dtype>::~FrcnnRoiDataLayer<Dtype>() {
  this->StopInternalThread();
}

template <typename Dtype>
void FrcnnRoiDataLayer<Dtype>::DataLayerSetUp(
    const vector<Blob<Dtype> *> &bottom, const vector<Blob<Dtype> *> &top) {
  // roi_data_file format
  // repeated:
  //   # image_index
  //   img_path (rel path)
  //   num_roi
  //   label x1 y1 x2 y2
  
  std::string default_config_file = this->layer_param_.window_data_param().config();
  FrcnnParam::load_param(default_config_file);
  FrcnnParam::print_param();
  cache_images_ = this->layer_param_.window_data_param().cache_images();

  LOG(INFO) << "FrcnnRoiDataLayer :" ;
  LOG(INFO) << "  source file :"
            << this->layer_param_.window_data_param().source() ; 
  LOG(INFO) << "  cache_images: "
            << ( cache_images_ ? "true" : "false" ) ; 
  LOG(INFO) << "  root_folder: "
            << this->layer_param_.window_data_param().root_folder() ;
  LOG(INFO) << "  Default Config File: "
            << default_config_file ;

  const std::string root_folder =
      this->layer_param_.window_data_param().root_folder();

  std::ifstream infile(this->layer_param_.window_data_param().source().c_str());
  CHECK(infile.good()) << "Failed to open roi_data file "
                       << this->layer_param_.window_data_param().source()
                       << std::endl;

  map<int, int> label_hist;
  label_hist.insert(std::make_pair(0, 0));
  roi_database_.clear();

  DataPrepare data_load;
  while( data_load.load_WithDiff(infile) ) {
    string image_path = data_load.GetImagePath(root_folder);
    //int image_index = data_load.GetImageIndex();
    image_database_.push_back(image_path);
    lines_.push_back(image_database_.size()-1);
    if (cache_images_) {
      Datum datum;
      if (!ReadFileToDatum(image_path, &datum)) {
        LOG(ERROR) << "Could not open or find file " << image_path;
        return;
      }
      image_database_cache_.push_back(std::make_pair(image_path, datum));
    }
    //vector<vector<float> > rois = data_load.GetRois( false );
    vector<vector<float> > rois = data_load.GetRois( true );//include difficulty GT rois
    for (size_t i = 0; i < rois.size(); ++i) {
      int label = rois[i][DataPrepare::LABEL];
      label_hist.insert(std::make_pair(label, 0));
      label_hist[label]++;
    }
    roi_database_.push_back(rois);
    if (lines_.size() % 1000 == 0) {
        LOG(INFO) << "num: " << lines_.size() << " " << image_path << " "
            << "rois to process: " << rois.size();
    }
  }

  CHECK_GT(lines_.size(), 0) << "No Image In Ground Truth File";
  LOG(INFO) << "number of images: " << lines_.size();

  for (map<int, int>::iterator it = label_hist.begin(); it != label_hist.end(); ++it) {
    LOG(INFO) << "class " << it->first << " has " << label_hist[it->first] << " samples";
  }

  // image
  vector<float> scales = FrcnnParam::scales;
  max_short_ = *max_element(scales.begin(), scales.end());
  max_long_ = FrcnnParam::max_size;
  const int batch_size = 1;

  // data mean
  for (int i = 0; i < 3; i++) {
    mean_values_[i] = FrcnnParam::pixel_means[i];
  }

  // data image Input ..
  CHECK_GT(max_short_, 0);
  CHECK_GT(max_long_, 0);

  max_short_ = int(std::ceil(max_short_ / float(FrcnnParam::im_size_align)) * FrcnnParam::im_size_align);
  max_long_ = int(std::ceil(max_long_ / float(FrcnnParam::im_size_align)) * FrcnnParam::im_size_align);
  top[0]->Reshape(batch_size, 3, max_short_, max_long_);
  for (int i = 0; i < this->prefetch_.size(); ++i) {
    this->prefetch_[i]->data_.Reshape(batch_size, 3, max_short_, max_long_);
  }

  LOG(INFO) << "output data size: " << top[0]->num() << ","
            << top[0]->channels() << "," << top[0]->height() << ","
            << top[0]->width();

  // im_info: height width scale_factor
  top[1]->Reshape(1, 3, 1, 1);
  // gt_boxes: label x1 y1 x2 y2
  top[2]->Reshape(batch_size, 5, 1, 1);

  for (int i = 0; i < this->prefetch_.size(); ++i) {
    this->prefetch_[i]->label_.Reshape(batch_size + 1, 5, 1, 1);
  }

  LOG(INFO) << "Shuffling data";
  // fyk: if we use multi GPU
  // int solver_count = Caffe::solver_count();
  // Caffe::solver_rank() is set different value of each device by parallel.cpp 
  const unsigned int prefetch_rng_seed = FrcnnParam::rng_seed + Caffe::solver_rank();
  prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
  lines_id_ = 0; // First Shuffle
  CHECK(prefetch_rng_);
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  shuffle(lines_.begin(), lines_.end(), prefetch_rng);
} 

template <typename Dtype>
void FrcnnRoiDataLayer<Dtype>::ShuffleImages() {
  lines_id_++;
  if (lines_id_ >= lines_.size()) {
    // We have reached the end. Restart from the first.
    DLOG(INFO) << "Restarting data prefetching from start.";
    lines_id_ = 0;
    CHECK(prefetch_rng_);
    caffe::rng_t* prefetch_rng =
        static_cast<caffe::rng_t*>(prefetch_rng_->generator());
    shuffle(lines_.begin(), lines_.end(), prefetch_rng);
  }
}

template <typename Dtype>
unsigned int FrcnnRoiDataLayer<Dtype>::PrefetchRand() {
  CHECK(prefetch_rng_);
  caffe::rng_t *prefetch_rng =
      static_cast<caffe::rng_t *>(prefetch_rng_->generator());
  return (*prefetch_rng)();
}

template <typename Dtype>
void FrcnnRoiDataLayer<Dtype>::CheckResetRois(vector<vector<float> > &rois, const string image_path, const float cols, const float rows, const float im_scale) {
  CHECK_GT(rois.size(),0);//if there is no rois, will cause the following layer error
  for (int i = 0; i < rois.size(); i++) {
    bool ok = rois[i][DataPrepare::X1] >= 0 && rois[i][DataPrepare::Y1] >= 0 && rois[i][DataPrepare::X2] < cols && rois[i][DataPrepare::Y2] < rows;
    if (ok == false) {
      DLOG(INFO) << "Roi Data Check Failed : " << image_path << " [" << i << "]";
      DLOG(INFO) << " row : " << rows << ",  col : " << cols << ", im_scale : " << im_scale << " | " << rois[i][DataPrepare::X1] << ", " << rois[i][DataPrepare::Y1] << ", " << rois[i][DataPrepare::X2] << ", " << rois[i][DataPrepare::Y2];
      rois[i][DataPrepare::X1] = std::max(0.f, rois[i][DataPrepare::X1]);
      rois[i][DataPrepare::Y1] = std::max(0.f, rois[i][DataPrepare::Y1]);
      rois[i][DataPrepare::X2] = std::min(cols-1, rois[i][DataPrepare::X2]);
      rois[i][DataPrepare::Y2] = std::min(rows-1, rois[i][DataPrepare::Y2]);
    }
  }
}

template <typename Dtype>
void FrcnnRoiDataLayer<Dtype>::FlipRois(vector<vector<float> > &rois, const float cols) {
  for (int i = 0; i < rois.size(); i++) {
    CHECK_GE(rois[i][DataPrepare::X1], 0 ) << "rois[i][DataPrepare::X1] : " << rois[i][DataPrepare::X1];
    CHECK_LT(rois[i][DataPrepare::X2], cols ) << "rois[i][DataPrepare::X2] : " << rois[i][DataPrepare::X2];
    float old_x1 = rois[i][DataPrepare::X1];
    float old_x2 = rois[i][DataPrepare::X2];
    rois[i][DataPrepare::X1] = cols - old_x2 - 1; 
    rois[i][DataPrepare::X2] = cols - old_x1 - 1; 
  }
}

// This function is called on prefetch thread
template <typename Dtype>
void FrcnnRoiDataLayer<Dtype>::load_batch(Batch<Dtype> *batch) {
  // At each iteration, Give Batch images and
  CPUTimer batch_timer;
  batch_timer.Start();
  double read_time = 0;
  double trans_time = 0;
  CPUTimer timer;

  const vector<float> scales = FrcnnParam::scales;
  const bool mirror = FrcnnParam::use_flipped;
  const int batch_size = 1;

  timer.Start();
  CHECK_EQ(roi_database_.size(), image_database_.size())
      << "image and roi size abnormal";

  // Select id for batch -> <0 if fliped
  ShuffleImages();
  CHECK(lines_id_ < lines_.size() && lines_id_ >= 0) << "select error line id : " << lines_id_;
  int index = lines_[lines_id_];
  bool do_mirror = mirror && PrefetchRand() % 2 && this->phase_ == TRAIN;
  //bool do_augment = FrcnnParam::data_jitter >= 0 && PrefetchRand() % 2 && this->phase_ == TRAIN;
  bool do_augment = FrcnnParam::data_jitter >= 0 && this->phase_ == TRAIN;
  float max_short = scales[PrefetchRand() % scales.size()];

  read_time += timer.MicroSeconds();

  // Prepare Image and labels;
  timer.Start();
  cv::Mat cv_img;
  if (this->cache_images_) {
    pair<std::string, Datum> image_cached = image_database_cache_[index];
    cv_img = DecodeDatumToCVMat(image_cached.second, true);
  } else {
    cv_img = cv::imread(image_database_[index], CV_LOAD_IMAGE_COLOR);
    if (!cv_img.data) {
      LOG(FATAL) << "Could not open or find file " << image_database_[index];
      return;
    }
  }
  cv::Mat src;
  cv_img.convertTo(src, CV_32FC3);

  CHECK(src.isContinuous()) << "Warning : cv::Mat src is not Continuous !";
  CHECK_EQ(src.depth(), CV_32F) << "Image data type must be float 32 type";
  CHECK_EQ(src.channels(), 3) << "Image data type must be 3 channels";
  read_time += timer.MicroSeconds();

  timer.Start();
  vector<vector<float> > rois = roi_database_[index];
  // std::cout << image_database_[index] << std::endl;    
  // horizontal flip
  if (do_mirror) {
    cv::flip(src, src, 1); // Flip
    FlipRois(rois, cv_img.cols);
  }
  // other augmentation method
  if (do_augment) {
    cv::Mat mat_aug = src.clone();
    std::vector<std::vector<float> > aug_rois = rois;
    // yolo style
    //cv::Mat mat_aug = data_augment(mat_aug, aug_rois, do_mirror, FrcnnParam::data_jitter, FrcnnParam::data_rand_scale, FrcnnParam::data_rand_rotate, FrcnnParam::data_hue, FrcnnParam::data_saturation, FrcnnParam::data_exposure);
    float prob;
    int clockwise_degree = 0;
    if (FrcnnParam::data_rand_rotate) {
      caffe_rng_uniform(1, 0.f, 1.f, &prob);
      if(prob > 0.75) clockwise_degree = 90;
      else if(prob > 0.5) clockwise_degree = 180;
      else if(prob > 0.25) clockwise_degree = 270;
    }
    if (clockwise_degree > 0) {
      aug_rois = rotate_rois(src, rois, clockwise_degree, mat_aug);
    }
    if (FrcnnParam::data_jitter > 0) {
      aug_rois = shift_image(mat_aug, aug_rois, FrcnnParam::data_jitter, mat_aug);
    }
    // distort
    caffe_rng_uniform(1, 0.f, 1.f, &prob);
    // Do random saturation & exposure distortion.
    RandomSaturation(mat_aug, &mat_aug, prob, 1 / FrcnnParam::data_saturation, FrcnnParam::data_saturation);
    RandomExposure(mat_aug, &mat_aug, prob, 1 / FrcnnParam::data_exposure, FrcnnParam::data_exposure);

    // Do random hue distortion.
    float hue_delta = 256 * FrcnnParam::data_hue;
    RandomHue(mat_aug, &mat_aug, prob, hue_delta);
    // other: RandomBrightness RandomContrast RandomOrderChannels

    // remove predicted boxes with either height or width < threshold, same as proposal layer
    vector<vector<float> > rois_aug;
    for (int i = 0; i < aug_rois.size(); i++) {
        if ( (aug_rois[i][DataPrepare::X2] - aug_rois[i][DataPrepare::X1]) > FrcnnParam::rpn_min_size && (aug_rois[i][DataPrepare::Y2] - aug_rois[i][DataPrepare::Y1]) > FrcnnParam::rpn_min_size ) 
            rois_aug.push_back(aug_rois[i]);
    }
    // doing jitter may exclude the rois, and Faster R-CNN cannot handle the 0-roi data currently
    if (rois_aug.size() > 0) {
      //Mat tmp=mat_aug.clone();
      //for(int i=0;i<rois_aug.size();i++){
      //    std::cout << rois_aug[i][0] << ' ' << rois_aug[i][1] << ' ' << rois_aug[i][2] << ' ' << rois_aug[i][3] << ' ' << rois_aug[i][4] << std::endl;
      //    cvDrawDottedRect(tmp, cv::Point(rois_aug[i][1], rois_aug[i][2]), cv::Point(rois_aug[i][3], rois_aug[i][4]), cv::Scalar(0, 0, 200), 6, 1);
      //}
      //std::string im_name = std::to_string(index) + ".jpg";
      //cv::imwrite(im_name, tmp);
      cv_img = mat_aug;// used by CheckResetRois
      src = mat_aug;
      rois = rois_aug;
    }
  }
  //fyk: do haze free,NOTICE that data enhancement should only be done one, current prioty is haze-free > retinex > hist_equalize
  if (FrcnnParam::use_haze_free) {
    src = remove_haze(src);
    src.convertTo(src, CV_32FC3);
  }else if (FrcnnParam::use_retinex) {
  	// NOT_IMPLEMENTED
  }else{
    //fyk : do equlize_hist,only for 3-channel
    int he_case = FrcnnParam::use_hist_equalize;
    cv::Mat tmp_mat;
    src.convertTo(tmp_mat, CV_8UC3);
    switch(he_case) {
    case 1:
          src = equalizeIntensityHist(tmp_mat);
          break;
    case 2:
          src = equalizeChannelHist(tmp_mat);
          break;
    default:
          break;
    }
    if(he_case > 0) src.convertTo(src, CV_32FC3);
  }
  //fyk end
  // Convert by : sub means and resize
  // Image sub means
  for (int r = 0; r < src.rows; r++) {
    for (int c = 0; c < src.cols; c++) {
      int offset = (r * src.cols + c) * 3;
      reinterpret_cast<float *>(src.data)[offset + 0] -= this->mean_values_[0]; // B
      reinterpret_cast<float *>(src.data)[offset + 1] -= this->mean_values_[1]; // G
      reinterpret_cast<float *>(src.data)[offset + 2] -= this->mean_values_[2]; // R
    }
  }
  float im_scale = Frcnn::get_scale_factor(src.cols, src.rows, max_short, max_long_);
  //fyk: check decimation or zoom,use different method
  if( im_scale < 1 )
    cv::resize(src, src, cv::Size(), im_scale, im_scale, cv::INTER_AREA );
  else
    cv::resize(src, src, cv::Size(), im_scale, im_scale);
  if (FrcnnParam::im_size_align > 0) {
    // pad to align im_size_align
    int new_im_height = int(std::ceil(src.rows / float(FrcnnParam::im_size_align)) * FrcnnParam::im_size_align);
    int new_im_width = int(std::ceil(src.cols / float(FrcnnParam::im_size_align)) * FrcnnParam::im_size_align);
    //std::cout << "align size: "<< new_im_height << ' '<< new_im_width << std::endl;
    cv::Mat padded_im = cv::Mat::zeros(cv::Size(new_im_width, new_im_height), CV_32FC3);
    float *res_mat_data = (float *)src.data;
    float *new_mat_data = (float *)padded_im.data;
    for (int y = 0; y < src.rows; ++y)
        for (int x = 0; x < src.cols; ++x)
            for (int k = 0; k < 3; ++k)
                new_mat_data[(y * new_im_width + x) * 3 + k] = res_mat_data[(y * src.cols + x) * 3 + k];
    src = padded_im;
  }
  // resize data
  batch->data_.Reshape(batch_size, 3, src.rows, src.cols);
  Dtype *top_data = batch->data_.mutable_cpu_data();

  for (int r = 0; r < src.rows; r++) {
    for (int c = 0; c < src.cols; c++) {
      int cv_offset = (r * src.cols + c) * 3;
      int blob_shift = r * src.cols + c;
      top_data[0 * src.rows * src.cols + blob_shift] = reinterpret_cast<float *>(src.data)[cv_offset + 0];
      top_data[1 * src.rows * src.cols + blob_shift] = reinterpret_cast<float *>(src.data)[cv_offset + 1];
      top_data[2 * src.rows * src.cols + blob_shift] = reinterpret_cast<float *>(src.data)[cv_offset + 2];
    }
  }

  // Check and Reset rois
  CheckResetRois(rois, image_database_[index], cv_img.cols, cv_img.rows, im_scale);
  
  // label format:
  // labels x1 y1 x2 y2
  // special for frcnn , this first channel is -1 , width , height ,
  // width_with_pad , height_with_pad
  const int channels = rois.size() + 1;
  batch->label_.Reshape(channels, 5, 1, 1);
  Dtype *top_label = batch->label_.mutable_cpu_data();

  top_label[0] = src.rows; // height
  top_label[1] = src.cols; // width
  top_label[2] = im_scale; // im_scale: used to filter min size
  top_label[3] = 0;
  top_label[4] = 0;

  //CHECK_EQ(rois.size(), channels-1);
  for (int i = 1; i < channels; i++) {
    CHECK_EQ(rois[i-1].size(), DataPrepare::NUM);
    top_label[5 * i + 0] = rois[i-1][DataPrepare::X1] * im_scale; // x1
    top_label[5 * i + 1] = rois[i-1][DataPrepare::Y1] * im_scale; // y1
    top_label[5 * i + 2] = rois[i-1][DataPrepare::X2] * im_scale; // x2
    top_label[5 * i + 3] = rois[i-1][DataPrepare::Y2] * im_scale; // y2
    top_label[5 * i + 4] = rois[i-1][DataPrepare::LABEL];         // label

    if (top_label[5 * i + 3] >= top_label[0]) {
      DLOG(INFO) << mirror << " row : " << src.rows << ",  col : " << src.cols << ", im_scale : " << im_scale << " | " << rois[i-1][DataPrepare::Y2] << " , " << top_label[5 * i + 3];
      top_label[5 * i + 3] = top_label[0] - 1;
    }
    if (top_label[5 * i + 2] >= top_label[1]) {
      DLOG(INFO) << mirror << " row : " << src.rows << ",  col : " << src.cols << ", im_scale : " << im_scale << " | " << rois[i-1][DataPrepare::X2] << " , " << top_label[5 * i + 2];
      top_label[5 * i + 2] = top_label[1] - 1;
    }
    if (top_label[5 * i + 0] < 0) {
      top_label[5 * i + 0] = 0;
      DLOG(INFO) << mirror << " row : " << src.rows << ",  col : " << src.cols << ", im_scale : " << im_scale << " | " << rois[i-1][DataPrepare::X2] << " , " << top_label[5 * i + 2];
    }
    if (top_label[5 * i + 1] < 0) {
      top_label[5 * i + 1] = 0;
      DLOG(INFO) << mirror << " row : " << src.rows << ",  col : " << src.cols << ", im_scale : " << im_scale << " | " << rois[i-1][DataPrepare::Y2] << " , " << top_label[5 * i + 3];
    }

#ifdef DEBUG_OUTPUT
    if (do_augment) {
          cvDrawDottedRect(src, cv::Point(rois[i-1][DataPrepare::X1] * im_scale, rois[i-1][DataPrepare::Y1] * im_scale), cv::Point(rois[i-1][DataPrepare::X2] * im_scale, rois[i-1][DataPrepare::Y2] * im_scale), cv::Scalar(0, 0, 200), 6, 1);
    }
#endif
  }
#ifdef DEBUG_OUTPUT
  if (do_augment) {
      std::string im_name = std::to_string(index) + ".jpg";
      cv::imwrite(im_name, src);
  }
#endif

  trans_time += timer.MicroSeconds();

  batch_timer.Stop();
  DLOG(INFO) << "Prefetch batch: " << batch_timer.MilliSeconds() << " ms.";
  DLOG(INFO) << "     Read time: " << read_time / 1000 << " ms.";
  DLOG(INFO) << "Transform time: " << trans_time / 1000 << " ms.";
}

template <typename Dtype>
void FrcnnRoiDataLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  Batch<Dtype>* batch = this->prefetch_full_.pop("Data layer prefetch queue empty");
  // Reshape to loaded data.
  top[0]->ReshapeLike(batch->data_);
  // Copy the data
  caffe_copy(batch->data_.count(), batch->data_.cpu_data(), top[0]->mutable_cpu_data());
  if (this->output_labels_) {
    caffe_copy(3, batch->label_.cpu_data(), top[1]->mutable_cpu_data());
    // Reshape to loaded labels.
    top[2]->Reshape(batch->label_.num()-1, batch->label_.channels(), batch->label_.height(), batch->label_.width());
    // Copy the labels.
    caffe_copy(batch->label_.count() - 5, batch->label_.cpu_data() + 5, top[2]->mutable_cpu_data());
  }
  this->prefetch_free_.push(batch);
}

#ifdef CPU_ONLY
STUB_GPU_FORWARD(FrcnnRoiDataLayer, Forward);
#endif

INSTANTIATE_CLASS(FrcnnRoiDataLayer);
REGISTER_LAYER_CLASS(FrcnnRoiData);

} // namespace Frcnn

} // namespace caffe
