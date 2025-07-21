#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

struct tssdk_alsa_switch_ctl_ctx {
    snd_ctl_ext_t ext;
    snd_ctl_t *slave;
    int volume[2];
    int mute[2];
};

static int tssdk_alsa_switch_ctl_elem_count(snd_ctl_ext_t *ext)
{
    return 4;
}

static int tssdk_alsa_switch_ctl_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id)
{
    if (offset >= 4) {
        return -EINVAL;
    }
    
    snd_ctl_elem_id_clear(id);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    
    switch (offset) {
        case 0:
            snd_ctl_elem_id_set_name(id, "Master Playback Volume");
            snd_ctl_elem_id_set_index(id, 0);
            break;
        case 1:
            snd_ctl_elem_id_set_name(id, "Master Playback Switch");
            snd_ctl_elem_id_set_index(id, 0);
            break;
        case 2:
            snd_ctl_elem_id_set_name(id, "PCM Playback Volume");
            snd_ctl_elem_id_set_index(id, 0);
            break;
        case 3:
            snd_ctl_elem_id_set_name(id, "PCM Playback Switch");
            snd_ctl_elem_id_set_index(id, 0);
            break;
    }
    
    return 0;
}

static snd_ctl_ext_key_t tssdk_alsa_switch_ctl_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id)
{
    const char *name = snd_ctl_elem_id_get_name(id);
    
    if (std::strcmp(name, "Master Playback Volume") == 0) {
        return 0;
    } else if (std::strcmp(name, "Master Playback Switch") == 0) {
        return 1;
    } else if (std::strcmp(name, "PCM Playback Volume") == 0) {
        return 2;
    } else if (std::strcmp(name, "PCM Playback Switch") == 0) {
        return 3;
    }
    
    return -1;
}

static int tssdk_alsa_switch_ctl_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                                              int *type, unsigned int *acc, unsigned int *count)
{
    (void)ext;
    
    switch (key) {
        case 0:
        case 2:
            *type = SND_CTL_ELEM_TYPE_INTEGER;
            *acc = SND_CTL_EXT_ACCESS_READWRITE;
            *count = 2;
            break;
        case 1:
        case 3:
            *type = SND_CTL_ELEM_TYPE_BOOLEAN;
            *acc = SND_CTL_EXT_ACCESS_READWRITE;
            *count = 2;
            break;
        default:
            return -EINVAL;
    }
    
    return 0;
}

static int tssdk_alsa_switch_ctl_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                                                 long *imin, long *imax, long *istep)
{
    switch (key) {
        case 0:
        case 2:
            *imin = 0;
            *imax = 100;
            *istep = 1;
            break;
        default:
            return -EINVAL;
    }
    
    return 0;
}

static int tssdk_alsa_switch_ctl_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
    struct tssdk_alsa_switch_ctl_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctl_ctx*>(ext->private_data);
    
    switch (key) {
        case 0:
        case 2:
            value[0] = ctx->volume[0];
            value[1] = ctx->volume[1];
            break;
        case 1:
        case 3:
            value[0] = !ctx->mute[0];
            value[1] = !ctx->mute[1];
            break;
        default:
            return -EINVAL;
    }
    
    return 0;
}

static int tssdk_alsa_switch_ctl_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value)
{
    struct tssdk_alsa_switch_ctl_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctl_ctx*>(ext->private_data);
    
    switch (key) {
        case 0:
        case 2:
            ctx->volume[0] = value[0];
            ctx->volume[1] = value[1];
            break;
        case 1:
        case 3:
            ctx->mute[0] = !value[0];
            ctx->mute[1] = !value[1];
            break;
        default:
            return -EINVAL;
    }
    
    return 0;
}

static void tssdk_alsa_switch_ctl_close(snd_ctl_ext_t *ext)
{
    struct tssdk_alsa_switch_ctl_ctx *ctx = static_cast<struct tssdk_alsa_switch_ctl_ctx*>(ext->private_data);
    if (ctx->slave) {
        snd_ctl_close(ctx->slave);
    }
    std::free(ctx);
}

static snd_ctl_ext_callback_t tssdk_alsa_switch_ctl_ops = {
    .close = tssdk_alsa_switch_ctl_close,
    .elem_count = tssdk_alsa_switch_ctl_elem_count,
    .elem_list = tssdk_alsa_switch_ctl_elem_list,
    .find_elem = tssdk_alsa_switch_ctl_find_elem,
    .get_attribute = tssdk_alsa_switch_ctl_get_attribute,
    .get_integer_info = tssdk_alsa_switch_ctl_get_integer_info,
    .read_integer = tssdk_alsa_switch_ctl_read_integer,
    .write_integer = tssdk_alsa_switch_ctl_write_integer,
};

extern "C" {

SND_CTL_PLUGIN_DEFINE_FUNC(tssdk_alsa_switch)
{
    snd_config_iterator_t i, next;
    const char *slave_ctl = nullptr;
    struct tssdk_alsa_switch_ctl_ctx *ctx;
    int err;

    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;
        if (snd_config_get_id(n, &id) < 0)
            continue;
        if (std::strcmp(id, "comment") == 0 || std::strcmp(id, "type") == 0)
            continue;
        if (std::strcmp(id, "slavectl") == 0) {
            if (snd_config_get_string(n, &slave_ctl) < 0) {
                SNDERR("slavectl must be string");
                return -EINVAL;
            }
            continue;
        }
        SNDERR("Unknown field %s", id);
        return -EINVAL;
    }

    ctx = static_cast<struct tssdk_alsa_switch_ctl_ctx*>(std::calloc(1, sizeof(*ctx)));
    if (!ctx) {
        return -ENOMEM;
    }

    ctx->volume[0] = 100;
    ctx->volume[1] = 100;
    ctx->mute[0] = 0;
    ctx->mute[1] = 0;

    if (!slave_ctl || slave_ctl[0] == '\0') {
        slave_ctl = "default";
    }

    err = snd_ctl_open(&ctx->slave, slave_ctl, 0);
    if (err < 0) {
        ctx->slave = nullptr;
    }

    ctx->ext.version = SND_CTL_EXT_VERSION;
    ctx->ext.card_idx = 0;
    strcpy(ctx->ext.id, "TSSDK");
    strcpy(ctx->ext.driver, "TSSDK");
    strcpy(ctx->ext.name, "TSSDK ALSA Switch");
    strcpy(ctx->ext.longname, "TSSDK ALSA Switch Control Plugin");
    strcpy(ctx->ext.mixername, "TSSDK ALSA Switch");

    ctx->ext.callback = &tssdk_alsa_switch_ctl_ops;
    ctx->ext.private_data = ctx;

    err = snd_ctl_ext_create(&ctx->ext, name, 0);
    if (err < 0) {
        goto error;
    }

    *handlep = ctx->ext.handle;
    return 0;

error:
    if (ctx->slave) {
        snd_ctl_close(ctx->slave);
    }
    std::free(ctx);
    return err;
}

SND_CTL_PLUGIN_SYMBOL(tssdk_alsa_switch);

} 