#include <Video.h>
#include <VideoMode.h>
#include <VideoCamera.h>

#include <iostream>

using namespace v8;

extern "C" {
#include <libavdevice/avdevice.h>
}

namespace ffmpeg {

const int kBufferSize = 8 * 1024;
const int bpp = 4;
const AVPixelFormat kPixelFormat = AV_PIX_FMT_RGBA;

AppData::AppData() :
  dataPos(0),
  fmt_ctx(nullptr), io_ctx(nullptr), stream_idx(-1), video_stream(nullptr), codec_ctx(nullptr), decoder(nullptr), packet(nullptr), av_frame(nullptr), gl_frame(nullptr), conv_ctx(nullptr), lastTimestamp(0) {}
AppData::~AppData() {
  resetState();
}

void AppData::resetState() {
  if (av_frame) {
    av_free(av_frame);
    av_frame = nullptr;
  }
  if (gl_frame) {
    av_free(gl_frame);
    gl_frame = nullptr;
  }
  if (packet) {
    av_free_packet(packet);
    packet = nullptr;
  }
  if (codec_ctx) {
    avcodec_close(codec_ctx);
    codec_ctx = nullptr;
  }
  if (fmt_ctx) {
    avformat_free_context(fmt_ctx);
    fmt_ctx = nullptr;
  }
  if (io_ctx) {
    av_free(io_ctx->buffer);
    av_free(io_ctx);
    io_ctx = nullptr;
  }
}

bool AppData::set(vector<unsigned char> &&memory, string *error) {
  data = std::move(memory);
  resetState();

  // open video
  fmt_ctx = avformat_alloc_context();
  io_ctx = avio_alloc_context((unsigned char *)av_malloc(kBufferSize), kBufferSize, 0, this, bufferRead, nullptr, bufferSeek);
  fmt_ctx->pb = io_ctx;
  fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
  int ret = avformat_open_input(&fmt_ctx, "memory", nullptr, nullptr);
  if (ret < 0) {
    if (error) {
      *error = "failed to open input";
    }
    return false;
  }

  // find stream info
  ret = avformat_find_stream_info(fmt_ctx, nullptr);
  if (ret < 0) {
    if (error) {
      *error = "failed to get stream info: ";
      char errbuf[1024];
      av_strerror(ret, errbuf, sizeof(errbuf));
      *error += errbuf;
    }
    return false;
  }

  // dump debug info
  // av_dump_format(fmt_ctx, 0, argv[1], 0);

  // find the video stream
  stream_idx = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i)
  {
      if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
      {
          stream_idx = i;
          break;
      }
  }
  if (stream_idx == -1) {
    if (error) {
      *error = "failed to find video stream";
    }
    return false;
  }

  video_stream = fmt_ctx->streams[stream_idx];
  
  // find the decoder
  decoder = avcodec_find_decoder(video_stream->codec->codec_id);
  if (decoder == nullptr) {
    if (error) {
      *error = "failed to find decoder: ";
      *error += avcodec_get_name(video_stream->codec->codec_id);
    }
    return false;
  }

  // open the decoder
  codec_ctx = avcodec_alloc_context3(decoder);
  ret = avcodec_copy_context(codec_ctx, video_stream->codec);
  if (ret < 0) {
    if (error) {
      *error = "failed to copy codec context: ";
      char errbuf[1024];
      av_strerror(ret, errbuf, sizeof(errbuf));
      *error += errbuf;
    }
    return false;
  }
  ret = avcodec_open2(codec_ctx, decoder, nullptr);
  if (ret < 0) {
    if (error) {
      *error = "failed to open codec: ";
      char errbuf[1024];
      av_strerror(ret, errbuf, sizeof(errbuf));
      *error += errbuf;
    }
    return false;
  }

  // allocate the video frames
  av_frame = av_frame_alloc();
  gl_frame = av_frame_alloc();
  int size = avpicture_get_size(kPixelFormat, codec_ctx->width, codec_ctx->height);
  uint8_t *internal_buffer = (uint8_t *)av_malloc(size * sizeof(uint8_t));
  avpicture_fill((AVPicture *)gl_frame, internal_buffer, kPixelFormat, codec_ctx->width, codec_ctx->height);
  packet = (AVPacket *)av_malloc(sizeof(AVPacket));

  // allocate the converter
  conv_ctx = sws_getContext(
    codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt != -1 ? codec_ctx->pix_fmt : AV_PIX_FMT_YUV420P,
    codec_ctx->width, codec_ctx->height, kPixelFormat,
    SWS_BICUBIC, nullptr, nullptr, nullptr
  );

  return true;
}

int AppData::bufferRead(void *opaque, unsigned char *buf, int buf_size) {
  AppData *appData = (AppData *)opaque;
  int64_t readLength = std::min<int64_t>(buf_size, appData->data.size() - appData->dataPos);
  if (readLength > 0) {
    memcpy(buf, appData->data.data() + appData->dataPos, readLength);
    appData->dataPos += readLength;
    return readLength;
  } else {
    return AVERROR_EOF;
  }
}
int64_t AppData::bufferSeek(void *opaque, int64_t offset, int whence) {
  AppData *appData = (AppData *)opaque;
  if (whence == AVSEEK_SIZE) {
    return appData->data.size();
  } else {
    int64_t newPos;
    if (whence == SEEK_SET) {
      newPos = offset;
    } else if (whence == SEEK_CUR) {
      newPos = appData->dataPos + offset;
    } else if (whence == SEEK_END) {
      newPos = appData->data.size() + offset;
    } else {
      newPos = offset;
    }
    newPos = std::min<int64_t>(std::max<int64_t>(newPos, 0), appData->data.size() - appData->dataPos);
    appData->dataPos = newPos;
    return newPos;
  }
}

FrameStatus AppData::advanceToFrameAt(double timestamp) {
  double timeBase = getTimeBase();

  for (;;) {
    if (lastTimestamp >= timestamp) {
      return FRAME_STATUS_OK;
    }

    bool packetValid = false;
    int ret;
    for (;;) {
      if (packetValid) {
        av_free_packet(packet);
        packetValid = false;
      }

      ret = av_read_frame(fmt_ctx, packet);
      packetValid = true;
      if (ret == AVERROR_EOF) {
        av_free_packet(packet);
        return FRAME_STATUS_EOF;
      } else if (ret < 0) {
        // std::cout << "Unknown error " << ret << "\n";
        av_free_packet(packet);
        return FRAME_STATUS_ERROR;
      } else {
        if (packet->stream_index == stream_idx) {
          break;
        }
      }
    }
    // we have a valid packet at this point
    int frame_finished = 0;
    ret = avcodec_decode_video2(codec_ctx, av_frame, &frame_finished, packet);
    if (ret < 0) {
      av_free_packet(packet);
      return FRAME_STATUS_ERROR;
    }

    if (frame_finished) {
      sws_scale(conv_ctx, av_frame->data, av_frame->linesize, 0, codec_ctx->height, gl_frame->data, gl_frame->linesize);
      lastTimestamp = (double)packet->pts * timeBase;
    }

    av_free_packet(packet);
  }
}

double AppData::getTimeBase() {
  if (video_stream) {
    return (double)video_stream->time_base.num / (double)video_stream->time_base.den;
  } else {
    return 1;
  }
}

Video::Video() :
  loaded(false), playing(false), loop(false), startTime(0), startFrameTime(0)
{
  videos.push_back(this);

  uv_sem_init(&requestSem, 0);
  // uv_sem_init(&responseSem, 0);
  thread = std::thread([this]() -> void {
    for (;;) {
      uv_sem_wait(&this->requestSem);
      std::lock_guard<std::mutex> lock(this->requestMutex);

      if (this->requestQueue.size() > 0) {
        VideoRequest &entry = this->requestQueue.front();
        entry.fn([&](std::function<void()> cb) -> void {
          {
            std::lock_guard<std::mutex> lock(responseMutex);

            responseQueue.push_back(VideoResponse{cb});
          }

          uv_async_send(&responseAsync);
        });

        this->requestQueue.pop_front();
      } else {
        break;
      }
    }

    uv_sem_destroy(&this->requestSem);
  });
}

Video::~Video() {
  videos.erase(std::find(videos.begin(), videos.end(), this));
  uv_sem_post(&requestSem); // join the thread
  // uv_sem_destroy(&responseSem);
}

Handle<Object> Video::Initialize(Isolate *isolate) {
  av_register_all();
  avcodec_register_all();
  avdevice_register_all();
  avformat_network_init();

  uv_async_init(uv_default_loop(), &responseAsync, runInMainThread);

  Nan::EscapableHandleScope scope;

  // constructor
  Local<FunctionTemplate> ctor = Nan::New<FunctionTemplate>(New);
  ctor->InstanceTemplate()->SetInternalFieldCount(1);
  ctor->SetClassName(JS_STR("Video"));

  // prototype
  Local<ObjectTemplate> proto = ctor->PrototypeTemplate();
  Nan::SetMethod(proto, "load", Load);
  Nan::SetMethod(proto, "update", Update);
  Nan::SetMethod(proto, "play", Play);
  Nan::SetMethod(proto, "pause", Pause);
  Nan::SetAccessor(proto, JS_STR("width"), WidthGetter);
  Nan::SetAccessor(proto, JS_STR("height"), HeightGetter);
  Nan::SetAccessor(proto, JS_STR("loop"), LoopGetter, LoopSetter);
  Nan::SetAccessor(proto, JS_STR("onload"), OnLoadGetter, OnLoadSetter);
  Nan::SetAccessor(proto, JS_STR("onerror"), OnErrorGetter, OnErrorSetter);
  Nan::SetAccessor(proto, JS_STR("data"), DataGetter);
  Nan::SetAccessor(proto, JS_STR("currentTime"), CurrentTimeGetter, CurrentTimeSetter);
  Nan::SetAccessor(proto, JS_STR("duration"), DurationGetter);

  Local<Function> ctorFn = ctor->GetFunction();

  Nan::SetMethod(ctorFn, "updateAll", UpdateAll);
  Nan::SetMethod(ctorFn, "getDevices", GetDevices);

  return scope.Escape(ctorFn);
}

NAN_METHOD(Video::New) {
  Nan::HandleScope scope;

  Video *video = new Video();
  Local<Object> videoObj = info.This();
  video->Wrap(videoObj);

  info.GetReturnValue().Set(videoObj);
}

void Video::Load(unsigned char *bufferValue, size_t bufferLength) {
  // reset state
  loaded = false;
  dataArray.Reset();

  // initialize custom data structure
  std::vector<unsigned char> bufferData(bufferLength);
  memcpy(bufferData.data(), bufferValue, bufferLength);

  std::cout << "load 1" << std::endl;
  queueInVideoThread([this, bufferData(std::move(bufferData))](std::function<void(std::function<void()>)> cb) mutable -> void {
    std::cout << "load 2" << std::endl;
    
    std::string error;
    if (this->appData.set(std::move(bufferData), &error)) { // takes ownership of bufferData
      std::cout << "load 3" << std::endl;
    
      FrameStatus status = this->appData.advanceToFrameAt(0);

      cb([this, status]() -> void {
        std::cout << "load 4" << std::endl;
        
        if (status == FRAME_STATUS_OK) {
          std::cout << "load 5" << std::endl;
          
          this->loaded = true;
          
          unsigned int width = this->GetWidth();
          unsigned int height = this->GetHeight();
          unsigned int dataSize = width * height * bpp;
          Local<ArrayBuffer> arrayBuffer = ArrayBuffer::New(Isolate::GetCurrent(), this->appData.gl_frame->data[0], dataSize);
          Local<Uint8ClampedArray> uint8ClampedArray = Uint8ClampedArray::New(arrayBuffer, 0, arrayBuffer->ByteLength());
          this->dataArray.Reset(uint8ClampedArray);

          if (!this->onload.IsEmpty()) {
            std::cout << "load 6" << std::endl;
            
            Local<Function> onloadFn = Nan::New(this->onload);
            onloadFn->Call(Nan::Null(), 0, nullptr);
          }
        } else {
          if (!this->onerror.IsEmpty()) {
            Local<Function> onerrorFn = Nan::New(this->onerror);
            Local<Value> args[] = {
              JS_STR("failed to advance to first video frame"),
            };
            onerrorFn->Call(Nan::Null(), sizeof(args)/sizeof(args[0]), args);
          }
        }
      });
    } else {
      cb([this, error(std::move(error))]() -> void {
        if (!this->onerror.IsEmpty()) {
          Local<Function> onerrorFn = Nan::New(this->onerror);
          Local<Value> args[] = {
            JS_STR(error),
          };
          onerrorFn->Call(Nan::Null(), sizeof(args)/sizeof(args[0]), args);
        }
      });
    }
  });
}

void Video::Update() {
  if (loaded && playing) {
    bool shouldQueue;
    {
      std::lock_guard<std::mutex> lock(requestMutex);
      shouldQueue = requestQueue.size() < 2;
    }
    if (shouldQueue) {
      double requiredCurrentTime = getRequiredCurrentTimeS();

      queueInVideoThread([this, requiredCurrentTime](std::function<void(std::function<void()>)> cb) -> void {
        FrameStatus status = this->appData.advanceToFrameAt(requiredCurrentTime);

        if (status == FRAME_STATUS_EOF) {
          cb([this]() -> void {
            if (this->loop) {
              this->SeekTo(0);
            } else {
              this->Pause();
            }
          });
        }
      });
    }
  }
}

void Video::Play() {
  if (!playing) {
    playing = true;
    startTime = av_gettime();
    startFrameTime = getFrameCurrentTimeS();
  }
}

void Video::Pause() {
  playing = false;
}

void Video::SeekTo(double timestamp) {
  if (loaded) {
    startTime = av_gettime() - (int64_t)(timestamp * 1e6);
    double requiredCurrentTime = getRequiredCurrentTimeS();

    queueInVideoThread([this, timestamp, requiredCurrentTime](std::function<void(std::function<void()>)> cb) -> void {
      this->appData.dataPos = 0;
      if (av_seek_frame(this->appData.fmt_ctx, this->appData.stream_idx, (int64_t)(timestamp / this->appData.video_stream->time_base.num * this->appData.video_stream->time_base.den), AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(this->appData.codec_ctx);
        av_free(this->appData.av_frame);
        this->appData.av_frame = av_frame_alloc();
        this->appData.lastTimestamp = 0;

        this->appData.advanceToFrameAt(requiredCurrentTime);
      } else {
        std::cerr << "failed to seek to video frame" << std::endl;
      }
    });
  }
}

uint32_t Video::GetWidth() {
  if (loaded) {
    return appData.codec_ctx->width;
  } else {
    return 0;
  }
}

uint32_t Video::GetHeight() {
  if (loaded) {
    return appData.codec_ctx->height;
  } else {
    return 0;
  }
}

NAN_METHOD(Video::Load) {
  if (info[0]->IsArrayBuffer()) {
    Video *video = ObjectWrap::Unwrap<Video>(info.This());

    Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(info[0]);

    video->Load((uint8_t *)arrayBuffer->GetContents().Data(), arrayBuffer->ByteLength());
  } else if (info[0]->IsTypedArray()) {
    Video *video = ObjectWrap::Unwrap<Video>(info.This());

    Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(info[0]);
    Local<ArrayBuffer> arrayBuffer = arrayBufferView->Buffer();

    video->Load((unsigned char *)arrayBuffer->GetContents().Data() + arrayBufferView->ByteOffset(), arrayBufferView->ByteLength());
  } else {
    Nan::ThrowError("Video::Load: invalid arguments");
  }
}

NAN_METHOD(Video::Update) {
  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  video->Update();
}

NAN_METHOD(Video::Play) {
  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  video->Play();
}

NAN_METHOD(Video::Pause) {
  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  video->Pause();
}

NAN_GETTER(Video::WidthGetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  info.GetReturnValue().Set(JS_INT(video->GetWidth()));
}
NAN_GETTER(Video::HeightGetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  info.GetReturnValue().Set(JS_INT(video->GetHeight()));
}

NAN_GETTER(Video::LoopGetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  info.GetReturnValue().Set(JS_BOOL(video->loop));
}
NAN_SETTER(Video::LoopSetter) {
  // Nan::HandleScope scope;

  if (value->IsBoolean()) {
    Video *video = ObjectWrap::Unwrap<Video>(info.This());
    video->loop = value->BooleanValue();
  } else {
    Nan::ThrowError("loop: invalid arguments");
  }
}

NAN_GETTER(Video::OnLoadGetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  Local<Function> onloadFn = Nan::New(video->onload);
  info.GetReturnValue().Set(onloadFn);
}
NAN_SETTER(Video::OnLoadSetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  if (value->IsFunction()) {
    video->onload.Reset(Local<Function>::Cast(value));
  } else {
    video->onload.Reset();
  }
}

NAN_GETTER(Video::OnErrorGetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  Local<Function> onerrorFn = Nan::New(video->onerror);
  info.GetReturnValue().Set(onerrorFn);
}
NAN_SETTER(Video::OnErrorSetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  if (value->IsFunction()) {
    video->onerror.Reset(Local<Function>::Cast(value));
  } else {
    video->onerror.Reset();
  }
}

NAN_GETTER(Video::DataGetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());
  Local<Uint8ClampedArray> uint8ClampedArray = Nan::New(video->dataArray);
  info.GetReturnValue().Set(uint8ClampedArray);
}

NAN_GETTER(Video::CurrentTimeGetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());

  double currentTime = video->getFrameCurrentTimeS();
  info.GetReturnValue().Set(JS_NUM(currentTime));
}

NAN_SETTER(Video::CurrentTimeSetter) {
  // Nan::HandleScope scope;

  if (value->IsNumber()) {
    Video *video = ObjectWrap::Unwrap<Video>(info.This());

    double timestamp = value->NumberValue();
    video->SeekTo(timestamp);
  } else {
    Nan::ThrowError("currentTime: invalid arguments");
  }
}

NAN_GETTER(Video::DurationGetter) {
  // Nan::HandleScope scope;

  Video *video = ObjectWrap::Unwrap<Video>(info.This());

  if (video->loaded) {
    double duration = video->loaded ? ((double)video->appData.fmt_ctx->duration / (double)AV_TIME_BASE) : 1;
    info.GetReturnValue().Set(JS_NUM(duration));
  } else {
    info.GetReturnValue().Set(JS_NUM(0));
  }
}

NAN_METHOD(Video::UpdateAll) {
  for (auto i : videos) {
    i->Update();
  }
}

NAN_METHOD(Video::GetDevices) {
  // Nan::HandleScope scope;

  DeviceList devices;
  VideoMode::getDevices(devices);

  Local<Object> lst = Array::New(Isolate::GetCurrent());
  size_t i = 0;
  for (auto device : devices) {
    const DeviceString& id(device.first);
    const DeviceString& name(device.second);
    Local<Object> obj = Object::New(Isolate::GetCurrent());
    lst->Set(i++, obj);
    obj->Set(JS_STR("id"), JS_STR(id.c_str()));
    obj->Set(JS_STR("name"), JS_STR(name.c_str()));

    VideoModeList modes;
    VideoMode::getDeviceModes(modes, id);

    Local<Object> lst = Array::New(Isolate::GetCurrent());
    size_t j = 0;
    for (auto mode : modes) {
      Local<Object> obj = Object::New(Isolate::GetCurrent());
      lst->Set(j++, obj);
      obj->Set(JS_STR("width"), JS_NUM(mode.width));
      obj->Set(JS_STR("height"), JS_NUM(mode.height));
      obj->Set(JS_STR("fps"), JS_NUM(mode.FPS));
    }
    obj->Set(JS_STR("modes"), lst);
  }
  info.GetReturnValue().Set(lst);
}

double Video::getRequiredCurrentTimeS() {
  if (playing) {
    int64_t now = av_gettime();
    int64_t startTimeDiff = now - startTime;
    double startTimeDiffS = std::max<double>((double)startTimeDiff / 1e6, 0);
    return startFrameTime + startTimeDiffS;
  } else {
    return getFrameCurrentTimeS();
  }
}

double Video::getFrameCurrentTimeS() {
  if (loaded) {
    double pts = appData.av_frame ? (double)std::max<int64_t>(appData.av_frame->pts, 0) : 0;
    double timeBase = appData.getTimeBase();
    return pts * timeBase;
  } else {
    return 0;
  }
}

void Video::queueInVideoThread(std::function<void(std::function<void(std::function<void()>)>)> fn) {
  requestQueue.push_back(VideoRequest{fn});

  uv_sem_post(&requestSem);
}

void Video::runInMainThread(uv_async_t *handle) {
  Nan::HandleScope scope;

  std::lock_guard<std::mutex> lock(responseMutex);

  while (responseQueue.size() > 0) {
    VideoResponse &entry = responseQueue.front();
    entry.fn();

    responseQueue.pop_front();
  }
}

VideoDevice::VideoDevice() : dev(nullptr) {
  videoDevices.push_back(this);
}

VideoDevice::~VideoDevice() {
  if (dev) {
    VideoMode::close(dev);
  }
  videoDevices.erase(std::find(videoDevices.begin(), videoDevices.end(), this));
}

Handle<Object> VideoDevice::Initialize(Isolate *isolate, Local<Value> imageDataCons) {
  Nan::EscapableHandleScope scope;

  // constructor
  Local<FunctionTemplate> ctor = Nan::New<FunctionTemplate>(New);
  ctor->InstanceTemplate()->SetInternalFieldCount(1);
  ctor->SetClassName(JS_STR("VideoDevice"));

  // prototype
  Local<ObjectTemplate> proto = ctor->PrototypeTemplate();
  Nan::SetMethod(proto, "open", Open);
  Nan::SetMethod(proto, "close", Close);
  Nan::SetAccessor(proto, JS_STR("width"), WidthGetter);
  Nan::SetAccessor(proto, JS_STR("height"), HeightGetter);
  Nan::SetAccessor(proto, JS_STR("size"), SizeGetter);
  Nan::SetAccessor(proto, JS_STR("data"), DataGetter);
  Nan::SetAccessor(proto, JS_STR("imageData"), ImageDataGetter);

  Local<Function> ctorFn = ctor->GetFunction();
  ctorFn->Set(JS_STR("ImageData"), imageDataCons);

  return scope.Escape(ctorFn);
}

NAN_METHOD(VideoDevice::New) {
  Nan::HandleScope scope;

  VideoDevice *video = new VideoDevice();
  Local<Object> videoObj = info.This();
  video->Wrap(videoObj);

  info.GetReturnValue().Set(videoObj);
}

NAN_METHOD(VideoDevice::Open) {
  VideoDevice *video = ObjectWrap::Unwrap<VideoDevice>(info.This());
  if (video->dev) {
    VideoMode::close(video->dev);
    video->dev = nullptr;
    video->imageData.Reset();
  }
  if (!info[0]->IsString()) {
    Nan::ThrowError("VideoDevice.Open: pass in a device name");
  } else {
    Nan::Utf8String nameStr(info[0]);
    std::string name(*nameStr, nameStr.length());
    std::string opts;
    if (info[1]->IsString()) {
      Nan::Utf8String optsStr(info[1]);
      opts = std::string(*optsStr, optsStr.length());
    }
    video->dev = VideoMode::open(name, opts);
    info.GetReturnValue().Set(JS_BOOL(video->dev != nullptr));
  }
}

NAN_METHOD(VideoDevice::Close) {
  VideoDevice *video = ObjectWrap::Unwrap<VideoDevice>(info.This());
  if (video->dev) {
    VideoMode::close(video->dev);
    video->dev = nullptr;
    video->imageData.Reset();
  }
}

NAN_GETTER(VideoDevice::WidthGetter) {
  Nan::HandleScope scope;

  VideoDevice *video = ObjectWrap::Unwrap<VideoDevice>(info.This());
  if (video->dev) {
    info.GetReturnValue().Set(JS_INT((unsigned int)video->dev->getWidth()));
  } else {
    info.GetReturnValue().Set(JS_INT(0));
  }
}

NAN_GETTER(VideoDevice::HeightGetter) {
  Nan::HandleScope scope;

  VideoDevice *video = ObjectWrap::Unwrap<VideoDevice>(info.This());
  if (video->dev) {
    info.GetReturnValue().Set(JS_INT((unsigned int)video->dev->getHeight()));
  } else {
    info.GetReturnValue().Set(JS_INT(0));
  }
}

NAN_GETTER(VideoDevice::SizeGetter) {
  Nan::HandleScope scope;

  VideoDevice *video = ObjectWrap::Unwrap<VideoDevice>(info.This());
  if (video->dev) {
    info.GetReturnValue().Set(JS_INT((unsigned int)video->dev->getSize()));
  } else {
    info.GetReturnValue().Set(JS_INT(0));
  }
}

NAN_GETTER(VideoDevice::DataGetter) {
  Nan::HandleScope scope;

  VideoDevice *video = ObjectWrap::Unwrap<VideoDevice>(info.This());

  if (video->dev && video->dev->isFrameReady()) {
    if (video->imageData.IsEmpty()) {
      double w = video->dev->getWidth();
      double h = video->dev->getHeight();

      Local<Function> imageDataCons = Local<Function>::Cast(
          Local<Object>::Cast(info.This())->Get(JS_STR("constructor"))->ToObject()->Get(JS_STR("ImageData"))
          );
      Local<Value> argv[] = {
        Number::New(Isolate::GetCurrent(), w),
        Number::New(Isolate::GetCurrent(), h),
      };
      video->imageData.Reset(imageDataCons->NewInstance(Isolate::GetCurrent()->GetCurrentContext(), sizeof(argv)/sizeof(argv[0]), argv).ToLocalChecked());
    }

    auto data = Nan::New(video->imageData)->Get(JS_STR("data"));
    if (data->IsUint8ClampedArray()) {
      auto uint8ClampedArray = Uint8ClampedArray::Cast(*data);
      uint8_t *buffer = (uint8_t *)uint8ClampedArray->Buffer()->GetContents().Data() + uint8ClampedArray->ByteOffset();
      video->dev->pullUpdate(buffer);
    }
  }

  if (!video->imageData.IsEmpty()) {
    info.GetReturnValue().Set(Nan::New(video->imageData)->Get(JS_STR("data")));
  } else {
    info.GetReturnValue().Set(Nan::Null());
  }
}

NAN_GETTER(VideoDevice::ImageDataGetter) {
  Nan::HandleScope scope;

  VideoDevice *video = ObjectWrap::Unwrap<VideoDevice>(info.This());

  if (!video->imageData.IsEmpty()) {
    info.GetReturnValue().Set(Nan::New(video->imageData));
  } else {
    info.GetReturnValue().Set(Nan::Null());
  }
}

std::mutex responseMutex;
std::deque<VideoResponse> responseQueue;
uv_async_t responseAsync;
std::vector<Video *> videos;
std::vector<VideoDevice *> videoDevices;

}
