#include "api/FRCNN/frcnn_api.hpp"
#include "caffe/FRCNN/util/frcnn_gpu_nms.hpp"
#include "api/util/blowfish.hpp"
#include <cstdio>

namespace FRCNN_API{

using namespace caffe::Frcnn;

void Detector::preprocess(const cv::Mat &img_in, const int blob_idx) {
  const vector<Blob<float> *> &input_blobs = net_->input_blobs();
  CHECK(img_in.isContinuous()) << "Warning : cv::Mat img_out is not Continuous !";
  DLOG(ERROR) << "img_in (CHW) : " << img_in.channels() << ", " << img_in.rows << ", " << img_in.cols; 
  input_blobs[blob_idx]->Reshape(1, img_in.channels(), img_in.rows, img_in.cols);
  float *blob_data = input_blobs[blob_idx]->mutable_cpu_data();
  const int cols = img_in.cols;
  const int rows = img_in.rows;
  for (int i = 0; i < cols * rows; i++) {
    blob_data[cols * rows * 0 + i] =
        reinterpret_cast<float*>(img_in.data)[i * 3 + 0] ;// mean_[0]; 
    blob_data[cols * rows * 1 + i] =
        reinterpret_cast<float*>(img_in.data)[i * 3 + 1] ;// mean_[1];
    blob_data[cols * rows * 2 + i] =
        reinterpret_cast<float*>(img_in.data)[i * 3 + 2] ;// mean_[2];
  }
}

void Detector::preprocess(const vector<float> &data, const int blob_idx) {
  const vector<Blob<float> *> &input_blobs = net_->input_blobs();
  input_blobs[blob_idx]->Reshape(1, data.size(), 1, 1);
  float *blob_data = input_blobs[blob_idx]->mutable_cpu_data();
  std::memcpy(blob_data, &data[0], sizeof(float) * data.size());
}

void Detector::Set_Model(std::string &proto_file, std::string &model_file) {
  // decypt the model, the key is fixed here. maybe you can place it somewhere else.
  if (FrcnnParam::test_decrypt_model) {
    // the key is love
    const char key[]  = {108, 111, 118, 101};
    vector<char> v_key(key, key + sizeof(key)/sizeof(char));
    Blowfish bf(v_key);
    std::string tmp_file = bf.getRandomTmpFile();
    bf.Decrypt(proto_file.c_str(), tmp_file.c_str());
    net_.reset(new Net<float>(tmp_file, caffe::TEST));
    bf.Decrypt(model_file.c_str(), tmp_file.c_str());
    net_->CopyTrainedLayersFrom(tmp_file);
    // rm the tmp file
    remove(tmp_file.c_str());
  } else {
    net_.reset(new Net<float>(proto_file, caffe::TEST));
    net_->CopyTrainedLayersFrom(model_file);
  }
  mean_[0] = FrcnnParam::pixel_means[0];
  mean_[1] = FrcnnParam::pixel_means[1];
  mean_[2] = FrcnnParam::pixel_means[2];
  const vector<std::string>& layer_names = this->net_->layer_names();
  const std::string roi_name = "roi_pool";
  this->roi_pool_layer = - 1;
  for (size_t i = 0; i < layer_names.size(); i++) {
    if (roi_name.size() > layer_names[i].size()) continue;
    if (roi_name == layer_names[i].substr(0, roi_name.size())) {
      //CHECK_EQ(this->roi_pool_layer, -1) << "Previous roi layer : " << this->roi_pool_layer << " : " << layer_names[this->roi_pool_layer];
      this->roi_pool_layer = i;
    }
  }
  // fyk: this var of roi_pool_layer is only used by predict_iterate,when I use 2 context roi_pool_layer or use R-FCN, I don't use predict_iterate
  //CHECK(this->roi_pool_layer >= 0 && this->roi_pool_layer < layer_names.size());
  DLOG(INFO) << "SET MODEL DONE, ROI POOLING LAYER : " << layer_names[this->roi_pool_layer];
  //caffe::Frcnn::FrcnnParam::print_param();
}

vector<boost::shared_ptr<Blob<float> > > Detector::predict(const vector<std::string> blob_names) {
  DLOG(ERROR) << "FORWARD BEGIN";
  float loss;
  net_->Forward(&loss);
  vector<boost::shared_ptr<Blob<float> > > output;
  for (int i = 0; i < blob_names.size(); ++i) {
    output.push_back(this->net_->blob_by_name(blob_names[i]));
  }
  DLOG(ERROR) << "FORWARD END, Loss : " << loss;
  return output;
}

void Detector::predict(const cv::Mat &img_in, std::vector<caffe::Frcnn::BBox<float> > &results) {
  CHECK(FrcnnParam::iter_test == -1 || FrcnnParam::iter_test > 1) << "FrcnnParam::iter_test == -1 || FrcnnParam::iter_test > 1";
  if (FrcnnParam::iter_test == -1) {
    predict_original(img_in, results);
  } else {
    predict_iterative(img_in, results);
  }
}

void Detector::predict_original(const cv::Mat &img_in, std::vector<caffe::Frcnn::BBox<float> > &results) {

  //CHECK(FrcnnParam::test_scales.size() == 1) << "Only single-image batch implemented";

std::vector<std::vector<caffe::Frcnn::BBox<float> > > bboxes_by_class(caffe::Frcnn::FrcnnParam::n_classes);
for (int test_scale_idx = 0; test_scale_idx < FrcnnParam::test_scales.size(); test_scale_idx++) {
  float scale_factor = caffe::Frcnn::get_scale_factor(img_in.cols, img_in.rows, FrcnnParam::test_scales[test_scale_idx], FrcnnParam::test_max_size);

  cv::Mat img;
  const int height = img_in.rows;
  const int width = img_in.cols;
  DLOG(INFO) << "height: " << height << " width: " << width;
  img_in.convertTo(img, CV_32FC3);
  for (int r = 0; r < img.rows; r++) {
    for (int c = 0; c < img.cols; c++) {
      int offset = (r * img.cols + c) * 3;
      reinterpret_cast<float *>(img.data)[offset + 0] -= this->mean_[0]; // B
      reinterpret_cast<float *>(img.data)[offset + 1] -= this->mean_[1]; // G
      reinterpret_cast<float *>(img.data)[offset + 2] -= this->mean_[2]; // R
    }
  }
  //cv::resize(img, img, cv::Size(), scale_factor, scale_factor);
  //fyk: check decimation or zoom,use different method
  if( scale_factor < 1 )
    cv::resize(img, img, cv::Size(), scale_factor, scale_factor, cv::INTER_AREA);
  else
    cv::resize(img, img, cv::Size(), scale_factor, scale_factor);
  if (FrcnnParam::im_size_align > 0) {
    // pad to align im_size_align
    int new_im_height = int(std::ceil(img.rows / float(FrcnnParam::im_size_align)) * FrcnnParam::im_size_align);
    int new_im_width = int(std::ceil(img.cols / float(FrcnnParam::im_size_align)) * FrcnnParam::im_size_align);
    cv::Mat padded_im = cv::Mat::zeros(cv::Size(new_im_width, new_im_height), CV_32FC3);
    float *res_mat_data = (float *)img.data;
    float *new_mat_data = (float *)padded_im.data;
    for (int y = 0; y < img.rows; ++y)
        for (int x = 0; x < img.cols; ++x)
            for (int k = 0; k < 3; ++k)
                new_mat_data[(y * new_im_width + x) * 3 + k] = res_mat_data[(y * img.cols + x) * 3 + k];
    img = padded_im;
  }

  std::vector<float> im_info(3);
  im_info[0] = img.rows;
  im_info[1] = img.cols;
  im_info[2] = scale_factor;

  DLOG(ERROR) << "im_info : " << im_info[0] << ", " << im_info[1] << ", " << im_info[2];
  this->preprocess(img, 0);
  this->preprocess(im_info, 1);

  vector<std::string> blob_names(3);
  blob_names[0] = "rois";
  blob_names[1] = "cls_prob";
  blob_names[2] = "bbox_pred";

  vector<boost::shared_ptr<Blob<float> > > output = this->predict(blob_names);
  boost::shared_ptr<Blob<float> > rois(output[0]);
  boost::shared_ptr<Blob<float> > cls_prob(output[1]);
  boost::shared_ptr<Blob<float> > bbox_pred(output[2]);

  const int box_num = bbox_pred->num();
  const int cls_num = cls_prob->channels();
  CHECK_EQ(cls_num , caffe::Frcnn::FrcnnParam::n_classes);
  results.clear();

  static float zero_means[] = {0.0, 0.0, 0.0, 0.0};
  static float one_stds[] = {1.0, 1.0, 1.0, 1.0};
  static float* means = FrcnnParam::bbox_normalize_targets ? FrcnnParam::bbox_normalize_means : zero_means;
  static float* stds  = FrcnnParam::bbox_normalize_targets ? FrcnnParam::bbox_normalize_stds : one_stds;
  for (int cls = 1; cls < cls_num; cls++) { 
    vector<BBox<float> >& bbox = bboxes_by_class[cls];
    for (int i = 0; i < box_num; i++) { 
      float score = cls_prob->cpu_data()[i * cls_num + cls];
      // fyk: speed up
      if (score < FrcnnParam::test_score_thresh) continue;

      Point4f<float> roi(rois->cpu_data()[(i * 5) + 1]/scale_factor,
                     rois->cpu_data()[(i * 5) + 2]/scale_factor,
                     rois->cpu_data()[(i * 5) + 3]/scale_factor,
                     rois->cpu_data()[(i * 5) + 4]/scale_factor);

      Point4f<float> delta(bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 0] * stds[0] + means[0],
                     bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 1] * stds[1] + means[1],
                     bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 2] * stds[2] + means[2],
                     bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 3] * stds[3] + means[3]);

      Point4f<float> box = caffe::Frcnn::bbox_transform_inv(roi, delta);
      //fyk clip predicted boxes to image
      box[0] = std::max(0.0f, std::min(box[0], width - 1.f));
      box[1] = std::max(0.0f, std::min(box[1], height - 1.f));
      box[2] = std::max(0.0f, std::min(box[2], width - 1.f));
      box[3] = std::max(0.0f, std::min(box[3], height - 1.f));

      // BBox tmp(box, score, cls);
      // LOG(ERROR) << "cls: " << tmp.id << " score: " << tmp.confidence;
      // LOG(ERROR) << "roi: " << roi.to_string();
      bbox.push_back(BBox<float>(box, score, cls));
    }
  } //class
}//scales
  int cls_num = caffe::Frcnn::FrcnnParam::n_classes;
  for (int cls = 1; cls < cls_num; cls++) { 
    vector<BBox<float> >& bbox = bboxes_by_class[cls];
    if (0 == bbox.size()) continue;
    vector<BBox<float> > bbox_backup = bbox;
    vector<BBox<float> > bbox_NMS;
    
    // Apply NMS
    // fyk: GPU nms
#ifndef CPU_ONLY
    if (caffe::Caffe::mode() == caffe::Caffe::GPU && FrcnnParam::test_use_gpu_nms) {
      int n_boxes = bbox.size();
      int box_dim = 5;
      // sort score if use naive nms
      if (FrcnnParam::test_soft_nms == 0) {
        sort(bbox.begin(), bbox.end());
        box_dim = 4;
      }
      std::vector<float> boxes_host(n_boxes * box_dim);
      for (int i=0; i < n_boxes; i++) {
        for (int k=0; k < box_dim; k++)
          boxes_host[i * box_dim + k] = bbox[i][k];
      }
      int keep_out[n_boxes];//keeped index of boxes_host
      int num_out;//how many boxes are keeped
      // call gpu nms, currently only support naive nms
      _nms(&keep_out[0], &num_out, &boxes_host[0], n_boxes, box_dim, FrcnnParam::test_nms);
      //if (FrcnnParam::test_soft_nms == 0) { // naive nms
      //  _nms(&keep_out[0], &num_out, &boxes_host[0], n_boxes, box_dim, FrcnnParam::test_nms);
      //} else {
      //  _soft_nms(&keep_out[0], &num_out, &boxes_host[0], n_boxes, box_dim, FrcnnParam::test_nms, FrcnnParam::test_soft_nms);
      //}
      for (int i=0; i < num_out; i++) {
        bbox_NMS.push_back(bbox[keep_out[i]]);
      }
    } else { // cpu
#endif
      if (FrcnnParam::test_soft_nms == 0) { // naive nms
        sort(bbox.begin(), bbox.end());
        vector<bool> select(bbox.size(), true);
        for (int i = 0; i < bbox.size(); i++)
          if (select[i]) {
            //if (bbox[i].confidence < FrcnnParam::test_score_thresh) break;
            for (int j = i + 1; j < bbox.size(); j++) {
              if (select[j]) {
                if (get_iou(bbox[i], bbox[j]) > FrcnnParam::test_nms) {
                  select[j] = false;
                }
              }
            }
            bbox_NMS.push_back(bbox[i]);
          }
      } else {
        // soft-nms
        float sigma = 0.5;
        float score_thresh = 0.001;
        int N = bbox.size();
        for (int cur_box_idx = 0; cur_box_idx < N; cur_box_idx++) {
          // find max score box
          float maxscore = bbox[cur_box_idx].confidence;
          int maxpos = cur_box_idx;
          for (int i = cur_box_idx + 1; i < N; i++) {
            if (maxscore < bbox[i].confidence) {
              maxscore = bbox[i].confidence;
              maxpos = i;
            }
          }
          //swap
          BBox<float> tt = bbox[cur_box_idx];
          bbox[cur_box_idx] = bbox[maxpos];
          bbox[maxpos] = tt;

          for (int i = cur_box_idx + 1; i < N; i++) {
            float iou = get_iou(bbox[i], bbox[cur_box_idx]);
            float weight = 1;
            if (1 == FrcnnParam::test_soft_nms) { // linear
              if (iou > FrcnnParam::test_nms) weight = 1 - iou;
            } else if (2 == FrcnnParam::test_soft_nms) { // gaussian
              weight = exp(- (iou * iou) / sigma);
            } else { // original NMS
              if (iou > FrcnnParam::test_nms) weight = 0;
            }
            bbox[i].confidence *= weight;
            if (bbox[i].confidence < score_thresh) {
              // discard the box by swapping with last box
              tt = bbox[i];
              bbox[i] = bbox[N-1];
              bbox[N-1] = tt;
              N -= 1;
              i -= 1;
            }
          }
        }
        for (int i=0; i < N; i++) {
          if (bbox[i].confidence >= FrcnnParam::test_score_thresh)
            bbox_NMS.push_back(bbox[i]);
        }
      } //nms type switch
#ifndef CPU_ONLY
    } //cpu
#endif
    // box-voting
    if (FrcnnParam::test_bbox_vote) {
      // since soft nms will change score of bbox, we use backup
      bbox_NMS = bbox_vote(bbox_NMS, bbox_backup);
      //bbox_NMS = bbox_vote(bbox_NMS, bbox_NMS);
    }

    results.insert(results.end(), bbox_NMS.begin(), bbox_NMS.end());
  }

}

void Detector::predict_iterative(const cv::Mat &img_in, std::vector<caffe::Frcnn::BBox<float> > &results) {

  CHECK(FrcnnParam::test_scales.size() == 1) << "Only single-image batch implemented";
  CHECK(FrcnnParam::iter_test >= 1) << "iter_test should greater and queal than 1";

  float scale_factor = caffe::Frcnn::get_scale_factor(img_in.cols, img_in.rows, FrcnnParam::test_scales[0], FrcnnParam::test_max_size);

  cv::Mat img;
  const int height = img_in.rows;
  const int width = img_in.cols;
  DLOG(INFO) << "height: " << height << " width: " << width;
  img_in.convertTo(img, CV_32FC3);
  for (int r = 0; r < img.rows; r++) {
    for (int c = 0; c < img.cols; c++) {
      int offset = (r * img.cols + c) * 3;
      reinterpret_cast<float *>(img.data)[offset + 0] -= this->mean_[0]; // B
      reinterpret_cast<float *>(img.data)[offset + 1] -= this->mean_[1]; // G
      reinterpret_cast<float *>(img.data)[offset + 2] -= this->mean_[2]; // R
    }
  }
  cv::resize(img, img, cv::Size(), scale_factor, scale_factor);

  std::vector<float> im_info(3);
  im_info[0] = img.rows;
  im_info[1] = img.cols;
  im_info[2] = scale_factor;

  DLOG(INFO) << "im_info : " << im_info[0] << ", " << im_info[1] << ", " << im_info[2];
  this->preprocess(img, 0);
  this->preprocess(im_info, 1);

  vector<std::string> blob_names(3);
  blob_names[0] = "rois";
  blob_names[1] = "cls_prob";
  blob_names[2] = "bbox_pred";
  
  vector<boost::shared_ptr<Blob<float> > > output = this->predict(blob_names);
  boost::shared_ptr<Blob<float> > rois(output[0]);
  boost::shared_ptr<Blob<float> > cls_prob(output[1]);
  boost::shared_ptr<Blob<float> > bbox_pred(output[2]);

  const int box_num = bbox_pred->num();
  const int cls_num = cls_prob->channels();
  CHECK_EQ(cls_num , caffe::Frcnn::FrcnnParam::n_classes);

  int iter_test = FrcnnParam::iter_test;
  while (--iter_test) {
    vector<BBox<float> > new_rois;
    for (int i = 0; i < box_num; i++) { 
      int cls_mx = 1;
      for (int cls = 1; cls < cls_num; cls++) { 
        float score    = cls_prob->cpu_data()[i * cls_num + cls];
        float mx_score = cls_prob->cpu_data()[i * cls_num + cls_mx];
        if (score >= mx_score) {
          cls_mx = cls;
        }
      }

      Point4f<float> roi(rois->cpu_data()[(i * 5) + 1],
                         rois->cpu_data()[(i * 5) + 2],
                         rois->cpu_data()[(i * 5) + 3],
                         rois->cpu_data()[(i * 5) + 4]);
#if 0
      new_rois.push_back( roi );
#endif

      Point4f<float> delta(bbox_pred->cpu_data()[(i * cls_num + cls_mx) * 4 + 0],
                           bbox_pred->cpu_data()[(i * cls_num + cls_mx) * 4 + 1],
                           bbox_pred->cpu_data()[(i * cls_num + cls_mx) * 4 + 2],
                           bbox_pred->cpu_data()[(i * cls_num + cls_mx) * 4 + 3]);

      Point4f<float> box = caffe::Frcnn::bbox_transform_inv(roi, delta);
      box[0] = std::max(0.0f, box[0]);
      box[1] = std::max(0.0f, box[1]);
      box[2] = std::min(im_info[1]-1.f, box[2]);
      box[3] = std::min(im_info[0]-1.f, box[3]);

      new_rois.push_back(box);
    }
    rois->Reshape(new_rois.size(), 5, 1, 1);
    for (size_t index = 0; index < new_rois.size(); index++) {
      rois->mutable_cpu_data()[ index * 5 ] = 0;
      for (int j = 1; j < 5; j++) {
        rois->mutable_cpu_data()[ index * 5 + j ] = new_rois[index][j-1];
      }
    }
    this->net_->ForwardFrom( this->roi_pool_layer );
    DLOG(INFO) << "iter_test[" << iter_test << "] >>> rois shape : " << rois->shape_string() << "  |  cls_prob shape : " << cls_prob->shape_string() << " | bbox_pred : " << bbox_pred->shape_string();
  }
  
  results.clear();

  for (int cls = 1; cls < cls_num; cls++) { 
    vector<BBox<float> > bbox;
    for (int i = 0; i < box_num; i++) { 
      float score = cls_prob->cpu_data()[i * cls_num + cls];

      Point4f<float> roi(rois->cpu_data()[(i * 5) + 1]/scale_factor,
                     rois->cpu_data()[(i * 5) + 2]/scale_factor,
                     rois->cpu_data()[(i * 5) + 3]/scale_factor,
                     rois->cpu_data()[(i * 5) + 4]/scale_factor);

      Point4f<float> delta(bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 0],
                     bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 1],
                     bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 2],
                     bbox_pred->cpu_data()[(i * cls_num + cls) * 4 + 3]);

      Point4f<float> box = caffe::Frcnn::bbox_transform_inv(roi, delta);
      box[0] = std::max(0.0f, box[0]);
      box[1] = std::max(0.0f, box[1]);
      box[2] = std::min(width-1.f, box[2]);
      box[3] = std::min(height-1.f, box[3]);

      bbox.push_back(BBox<float>(box, score, cls));
    }
    sort(bbox.begin(), bbox.end());
    vector<bool> select(box_num, true);
    // Apply NMS
    for (int i = 0; i < box_num; i++)
      if (select[i]) {
        if (bbox[i].confidence < FrcnnParam::test_score_thresh) break;
        for (int j = i + 1; j < box_num; j++) {
          if (select[j]) {
            if (get_iou(bbox[i], bbox[j]) > FrcnnParam::test_nms) {
              select[j] = false;
            }
          }
        }
        results.push_back(bbox[i]);
      }
  }

}

} // FRCNN_API
