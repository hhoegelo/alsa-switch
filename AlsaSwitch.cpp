#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <future>
#include <chrono>
#include <atomic>
#include <sys/eventfd.h>

using namespace std::chrono_literals;

static std::mutex mtx;
static int startCount = 0;

template<typename... Args>
void Log(Args... args) {
  std::unique_lock lock(mtx);
  (std::cout << ... << args) << std::endl;
}

struct tssdk_alsa_switch_ctx {
  snd_pcm_ioplug_t io;
  snd_pcm_t *slave;

  std::atomic_bool m_stop;
  std::future<void> m_bg;
  uint64_t readHead = 0;

  int notify_fd;

  tssdk_alsa_switch_ctx() {
    notify_fd = eventfd(0, EFD_NONBLOCK);
  }

  ~tssdk_alsa_switch_ctx() {
    stop();
    close(notify_fd);
  }

  void notify() {
    uint64_t val = 1;
    write(notify_fd, &val, sizeof(val));
  }

  void start() {
    Log("start: ", startCount);
    assert(startCount == 0);
    startCount++;

    m_bg = std::async(std::launch::async,
                      [this] {
                        while (!m_stop.exchange(false)) {
                          snd_pcm_uframes_t avail = snd_pcm_ioplug_hw_avail(&io, readHead, io.appl_ptr);

                          if (avail > 0) {
                            const snd_pcm_channel_area_t *areas = nullptr;
                            snd_pcm_uframes_t offset = 0;
                            snd_pcm_uframes_t frames = 0;

                            snd_pcm_mmap_begin(slave, &areas, &offset, &frames);
                            Log("Background: snd_pcm_mmap_begin: offset=", offset, " frames=", frames);

                            if (auto todo = std::min(frames, avail)) {
                              auto src = snd_pcm_ioplug_mmap_areas(&io);

                              snd_pcm_areas_copy_wrap(areas, offset, frames,
                                                      src,
                                                      io.appl_ptr % io.buffer_size, todo, io.channels, todo, io.format);

                              snd_pcm_mmap_commit(slave, offset, frames);
                            }
                          } else {
                            Log("Background: No frames available");
                            std::this_thread::sleep_for(1ms);
                          }

                          notify();
                        }
                      });
  }

  void stop() {
    Log("stop: ", startCount);
    assert(startCount == 1);
    m_stop.store(true);
    m_bg.wait();
    startCount--;
  }
};

static snd_pcm_sframes_t tssdk_alsa_switch_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas,
                                                    snd_pcm_uframes_t offset, snd_pcm_uframes_t size) {
  return 0;
}

static int tssdk_alsa_switch_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);

  Log("tssdk_alsa_switch_hw_params");

  snd_pcm_hw_params_t *slave_params;
  snd_pcm_hw_params_alloca(&slave_params);
  snd_pcm_hw_params_any(ctx->slave, slave_params);

  unsigned int rate;
  int dir;
  snd_pcm_hw_params_get_rate(params, &rate, nullptr);
  snd_pcm_hw_params_set_rate_near(ctx->slave, slave_params, &rate, &dir);

  snd_pcm_format_t format;
  snd_pcm_hw_params_get_format(params, &format);
  snd_pcm_hw_params_set_format(ctx->slave, slave_params, format);

  unsigned int channels;
  snd_pcm_hw_params_get_channels(params, &channels);
  snd_pcm_hw_params_set_channels(ctx->slave, slave_params, channels);

  snd_pcm_uframes_t period_size;
  snd_pcm_hw_params_get_period_size(params, &period_size, &dir);
  snd_pcm_hw_params_set_period_size_near(ctx->slave, slave_params, &period_size, &dir);

  snd_pcm_uframes_t buffer_size;
  snd_pcm_hw_params_get_buffer_size(params, &buffer_size);
  snd_pcm_hw_params_set_buffer_size_near(ctx->slave, slave_params, &buffer_size);

  snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
  snd_pcm_hw_params_set_access(ctx->slave, slave_params, access);
  snd_pcm_hw_params_get_access(params, &access);

  int result = snd_pcm_hw_params(ctx->slave, slave_params);

  if (result >= 0) {
    snd_pcm_hw_params_copy(params, slave_params);
  }

  return result;
}

static int tssdk_alsa_switch_hw_free(snd_pcm_ioplug_t *io) {
  Log("tssdk_alsa_switch_hw_free");
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  int result = snd_pcm_hw_free(ctx->slave);
  Log("tssdk_alsa_switch_hw_free: result=", result);
  return result;
}

static int tssdk_alsa_switch_prepare(snd_pcm_ioplug_t *io) {
  Log("tssdk_alsa_switch_prepare");
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  int result = snd_pcm_prepare(ctx->slave);
  Log("tssdk_alsa_switch_prepare: result=", result);
  return result;
}

static int tssdk_alsa_switch_drain(snd_pcm_ioplug_t *io) {
  Log("tssdk_alsa_switch_drain");
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  int result = snd_pcm_drain(ctx->slave);
  Log("tssdk_alsa_switch_drain: result=", result);
  return result;
}

static int tssdk_alsa_switch_start(snd_pcm_ioplug_t *io) {
  Log("tssdk_alsa_switch_start");
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  int result = snd_pcm_start(ctx->slave);
  Log("tssdk_alsa_switch_start: result=", result);
  ctx->start();
  return result;
}

static int tssdk_alsa_switch_stop(snd_pcm_ioplug_t *io) {
  Log("tssdk_alsa_switch_stop");
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  ctx->stop();
  return snd_pcm_drop(ctx->slave);
}

static snd_pcm_sframes_t tssdk_alsa_switch_pointer(snd_pcm_ioplug_t *io) {
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  Log("tssdk_alsa_switch_pointer, io=", io, " hw_ptr=", ctx->readHead, ", appl_ptr=", io->appl_ptr);
  return ctx->readHead;
}

static int tssdk_alsa_switch_close(snd_pcm_ioplug_t *io) {
  Log("tssdk_alsa_switch_close");
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  snd_pcm_t *slave = ctx->slave;
  if (slave) {
    ctx->slave = nullptr;
    int result = snd_pcm_close(slave);
    Log("tssdk_alsa_switch_close: result=", result);
  }

  delete ctx;
  return 0;
}

static int tssdk_alsa_switch_poll_descriptors_count(snd_pcm_ioplug_t *io) {
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  return ctx->notify_fd >= 0 ? 1 : 0;
}

static int tssdk_alsa_switch_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfds, unsigned int space) {
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  if (ctx->notify_fd >= 0 && space > 0) {
    pfds[0].fd = ctx->notify_fd;
    pfds[0].events = POLLIN;
    return 1;
  }
  return 0;
}

static int tssdk_alsa_switch_poll_descriptors_revents(snd_pcm_ioplug_t *io, struct pollfd *pfds, unsigned int nfds,
                                                      unsigned short *revents) {
  struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx *>(io->private_data);
  *revents = (pfds[0].revents & POLLIN) ? POLLOUT : 0;
  uint64_t temp;
  read(ctx->notify_fd, &temp, sizeof(temp)); // Clear eventfd state
  return 0;
}

static snd_pcm_ioplug_callback_t tssdk_alsa_switch_ops = {
  .start = tssdk_alsa_switch_start,
  .stop = tssdk_alsa_switch_stop,
  .pointer = tssdk_alsa_switch_pointer,
  .transfer = tssdk_alsa_switch_transfer,
  .close = tssdk_alsa_switch_close,
  .hw_params = tssdk_alsa_switch_hw_params,
  .hw_free = tssdk_alsa_switch_hw_free,
  .prepare = tssdk_alsa_switch_prepare,
  .drain = tssdk_alsa_switch_drain,
  .poll_descriptors_count = tssdk_alsa_switch_poll_descriptors_count,
  .poll_descriptors = tssdk_alsa_switch_poll_descriptors,
  .poll_revents = tssdk_alsa_switch_poll_descriptors_revents,
};

static tssdk_alsa_switch_ctx *tssdk_alsa_switch_new(void) {
  return new struct tssdk_alsa_switch_ctx;
}

static void tssdk_alsa_switch_delete(tssdk_alsa_switch_ctx *p) {
  delete p;
}

extern "C" {
SND_PCM_PLUGIN_DEFINE_FUNC(tssdk_alsa_switch) {
  Log("tssdk_alsa_switch: plugin initialization");
  snd_config_iterator_t i, next;
  const char *slave_pcm = nullptr;
  int err;

  if (stream != SND_PCM_STREAM_PLAYBACK) {
    Log("tssdk_alsa_switch: invalid stream type");
    return -EINVAL;
  }

  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if (snd_config_get_id(n, &id) < 0)
      continue;
    if (std::strcmp(id, "comment") == 0 || std::strcmp(id, "type") == 0)
      continue;
    if (std::strcmp(id, "slavepcm") == 0) {
      if (snd_config_get_string(n, &slave_pcm) < 0) {
        Log("tssdk_alsa_switch: slavepcm must be string");
        SNDERR("slavepcm must be string");
        return -EINVAL;
      }
      Log("tssdk_alsa_switch: slavepcm=", slave_pcm);
      continue;
    }
    Log("tssdk_alsa_switch: unknown field ", id);
    SNDERR("Unknown field %s", id);
    return -EINVAL;
  }

  auto ctx = tssdk_alsa_switch_new();
  Log("tssdk_alsa_switch: context allocated");

  if (!slave_pcm || slave_pcm[0] == '\0') {
    slave_pcm = "default";
    Log("tssdk_alsa_switch: using default slave PCM");
  }

  Log("tssdk_alsa_switch: opening slave PCM: ", slave_pcm);
  err = snd_pcm_open(&ctx->slave, slave_pcm, stream, mode);
  if (err < 0) {
    Log("tssdk_alsa_switch: failed to open slave PCM, error=", err);
    goto error;
  }
  Log("tssdk_alsa_switch: slave PCM opened successfully");

  ctx->io.version = SND_PCM_IOPLUG_VERSION;
  ctx->io.name = "TSSDK ALSA Switch Plugin";
  ctx->io.mmap_rw = 1;
  ctx->io.callback = &tssdk_alsa_switch_ops;
  ctx->io.private_data = ctx;

  Log("tssdk_alsa_switch: creating ioplug");
  err = snd_pcm_ioplug_create(&ctx->io, name, stream, mode);
  if (err < 0) {
    Log("tssdk_alsa_switch: failed to create ioplug, error=", err);
    goto error;
  }

  *pcmp = ctx->io.pcm;
  Log("tssdk_alsa_switch: plugin created successfully");
  return 0;

error:
  Log("tssdk_alsa_switch: error cleanup");
  if (ctx->slave) {
    Log("tssdk_alsa_switch: closing slave PCM");
    snd_pcm_close(ctx->slave);
  }
  Log("tssdk_alsa_switch: freeing context");
  tssdk_alsa_switch_delete(ctx);
  return err;
}

SND_PCM_PLUGIN_SYMBOL(tssdk_alsa_switch);
}
