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

struct tssdk_alsa_switch_ctx {
    snd_pcm_ioplug_t io;
    snd_pcm_t *slave;

    std::atomic_bool m_stop;
    std::future<void> m_bg;
    
    int notify_fd;

    tssdk_alsa_switch_ctx() {
        notify_fd = eventfd(0, EFD_NONBLOCK);
    }

    ~tssdk_alsa_switch_ctx() {
        stop();
        if (notify_fd >= 0) {
            close(notify_fd);
        }
    }

    void notify() {
        if (notify_fd >= 0) {
            uint64_t val = 1;
            write(notify_fd, &val, sizeof(val));
        }
    }

    void start() {
        m_bg = std::async (std::launch::async, [this] {

            while (!m_stop.exchange(false)) {
                            snd_pcm_uframes_t avail = snd_pcm_ioplug_hw_avail(&io, io.hw_ptr, io.appl_ptr);

                if (avail > 0) {
                    const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(&io);
                    int channels = io.channels;
                    int16_t buffer[avail * channels]; // adjust for format and layout
                    for (int ch = 0; ch < channels; ++ch) {
                        const snd_pcm_channel_area_t *area = &areas[ch];
                        uint8_t *src = (uint8_t *)area->addr + (area->first / 8) + ((io.hw_ptr * area->step) / 8);
                        for (snd_pcm_uframes_t frame = 0; frame < avail; ++frame) {
                            int index = frame * channels + ch;
                            buffer[index] = *(int16_t *)src; // assumes 16-bit samples
                            src += area->step / 8;
                        }
                    }

                    // Write to slave PCM device (assumes interleaved format)
                    snd_pcm_sframes_t written = snd_pcm_writei(slave, buffer, avail);
                    if (written > 0) {
                        io.hw_ptr = (io.hw_ptr + written) % io.buffer_size;
                    }
                }

                std::this_thread::sleep_for(1ms);
            }
        });
    }

    void stop() {
        m_stop.store(true);
        m_bg.wait();
    }
};

static snd_pcm_sframes_t tssdk_alsa_switch_transfer(snd_pcm_ioplug_t *io,
                                        const snd_pcm_channel_area_t *areas,
                                        snd_pcm_uframes_t offset,
                                        snd_pcm_uframes_t size)
{
  return 0;
}

static int tssdk_alsa_switch_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
    std::cout << "tssdk_alsa_switch_hw_params" << std::endl;

    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    
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

static int tssdk_alsa_switch_hw_free(snd_pcm_ioplug_t *io)
{
    std::cout << "tssdk_alsa_switch_hw_free" << std::endl;
    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    int result = snd_pcm_hw_free(ctx->slave);
    std::cout << "tssdk_alsa_switch_hw_free: result=" << result << std::endl;
    return result;
}

static int tssdk_alsa_switch_prepare(snd_pcm_ioplug_t *io)
{
    std::cout << "tssdk_alsa_switch_prepare" << std::endl;
    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    int result = snd_pcm_prepare(ctx->slave);
    std::cout << "tssdk_alsa_switch_prepare: result=" << result << std::endl;
    return result;
}

static int tssdk_alsa_switch_drain(snd_pcm_ioplug_t *io)
{
    std::cout << "tssdk_alsa_switch_drain" << std::endl;
    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    int result = snd_pcm_drain(ctx->slave);
    std::cout << "tssdk_alsa_switch_drain: result=" << result << std::endl;
    return result;
}

static int tssdk_alsa_switch_start(snd_pcm_ioplug_t *io)
{
    std::cout << "tssdk_alsa_switch_start" << std::endl;
    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    int result = snd_pcm_start(ctx->slave);
    std::cout << "tssdk_alsa_switch_start: result=" << result << std::endl;
    ctx->start();
    return result;
}

static int tssdk_alsa_switch_stop(snd_pcm_ioplug_t *io)
{
    std::cout << "tssdk_alsa_switch_stop" << std::endl;
    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    int result = snd_pcm_drop(ctx->slave);
    ctx->stop();
    std::cout << "tssdk_alsa_switch_stop: result=" << result << std::endl;
    return result;
}

static snd_pcm_sframes_t tssdk_alsa_switch_pointer(snd_pcm_ioplug_t *io)
{
    return io->hw_ptr - io->appl_ptr;

    auto suspend = access("/tmp/suspend-alsa-switch", F_OK) != -1;
    if (suspend) {
        std::cout << "tssdk_alsa_switch_pointer: suspended" << std::endl;
        return 0;
    }

    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    snd_pcm_sframes_t delay;
    int err = snd_pcm_delay(ctx->slave, &delay);
    if (err < 0) {
        std::cout << "tssdk_alsa_switch_pointer: error=" << err << std::endl;
        return err;
    }
    std::cout << "tssdk_alsa_switch_pointer: delay=" << delay << std::endl;
    return delay;
}

static int tssdk_alsa_switch_close(snd_pcm_ioplug_t *io)
{
    std::cout << "tssdk_alsa_switch_close" << std::endl;
    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    snd_pcm_t *slave = ctx->slave;
    if (slave) {
        ctx->slave = nullptr;
        int result = snd_pcm_close(slave);
        std::cout << "tssdk_alsa_switch_close: result=" << result << std::endl;
    }

    delete ctx;
    return 0;
}

static int tssdk_alsa_switch_poll_descriptors_count(snd_pcm_ioplug_t *io) {
    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    return ctx->notify_fd >= 0 ? 1 : 0;
}

static int tssdk_alsa_switch_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfds, unsigned int space) {
    struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    if (ctx->notify_fd >= 0 && space > 0) {
        pfds[0].fd = ctx->notify_fd;
        pfds[0].events = POLLOUT;
        return 1;
    }
    return 0;
}

static int tssdk_alsa_switch_poll_descriptors_revents(snd_pcm_ioplug_t *io, struct pollfd *pfds, unsigned int nfds, unsigned short *revents) {
    //struct tssdk_alsa_switch_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctx*>(io->private_data);
    *revents = POLLOUT;
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

SND_PCM_PLUGIN_DEFINE_FUNC(tssdk_alsa_switch)
{
    std::cout << "tssdk_alsa_switch: plugin initialization" << std::endl;
    snd_config_iterator_t i, next;
    const char *slave_pcm = nullptr;
    int err;

    if (stream != SND_PCM_STREAM_PLAYBACK) {
        std::cout << "tssdk_alsa_switch: invalid stream type" << std::endl;
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
                std::cout << "tssdk_alsa_switch: slavepcm must be string" << std::endl;
                SNDERR("slavepcm must be string");
                return -EINVAL;
            }
            std::cout << "tssdk_alsa_switch: slavepcm=" << slave_pcm << std::endl;
            continue;
        }
        std::cout << "tssdk_alsa_switch: unknown field " << id << std::endl;
        SNDERR("Unknown field %s", id);
        return -EINVAL;
    }

    auto ctx = tssdk_alsa_switch_new();
    std::cout << "tssdk_alsa_switch: context allocated" << std::endl;

    if (!slave_pcm || slave_pcm[0] == '\0') {
        slave_pcm = "default";
        std::cout << "tssdk_alsa_switch: using default slave PCM" << std::endl;
    }

    std::cout << "tssdk_alsa_switch: opening slave PCM: " << slave_pcm << std::endl;
    err = snd_pcm_open(&ctx->slave, slave_pcm, stream, mode);
    if (err < 0) {
        std::cout << "tssdk_alsa_switch: failed to open slave PCM, error=" << err << std::endl;
        goto error;
    }
    std::cout << "tssdk_alsa_switch: slave PCM opened successfully" << std::endl;

    ctx->io.version = SND_PCM_IOPLUG_VERSION;
    ctx->io.name = "TSSDK ALSA Switch Plugin";
    ctx->io.mmap_rw = 1;
    ctx->io.callback = &tssdk_alsa_switch_ops;
    ctx->io.private_data = ctx;

    std::cout << "tssdk_alsa_switch: creating ioplug" << std::endl;
    err = snd_pcm_ioplug_create(&ctx->io, name, stream, mode);
    if (err < 0) {
        std::cout << "tssdk_alsa_switch: failed to create ioplug, error=" << err << std::endl;
        goto error;
    }

    *pcmp = ctx->io.pcm;
    std::cout << "tssdk_alsa_switch: plugin created successfully" << std::endl;
    return 0;

error:
    std::cout << "tssdk_alsa_switch: error cleanup" << std::endl;
    if (ctx->slave) {
        std::cout << "tssdk_alsa_switch: closing slave PCM" << std::endl;
        snd_pcm_close(ctx->slave);
    }
    std::cout << "tssdk_alsa_switch: freeing context" << std::endl;
    tssdk_alsa_switch_delete(ctx);
    return err;
}

SND_PCM_PLUGIN_SYMBOL(tssdk_alsa_switch);

}