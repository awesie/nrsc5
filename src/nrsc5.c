#include <assert.h>
#include <SoapySDR/Device.h>
#include <string.h>
#include <unistd.h>

#include "private.h"

#define RX_CHAN 0
#define RX_BUFFER_FFT 16384
#define RX_BUFFER (RX_BUFFER_FFT * 4)
#define RX_TRANSITION_SAMPLES 81920
#define RX_TIMEOUT 5000000
#define AUTO_GAIN_STEP 4.0
#define AUTO_GAIN_MIN_PILOT 10.0
#define SCAN_MIN_SNR 2.0
#define SCAN_AUTO_GAIN_STEP 20.0

struct {
    const char *driver;
    float sample_rate;
    unsigned int decimation;
} supported_drivers[] = {
    { "rtlsdr", SAMPLE_RATE * 2, 2 },
    { "hackrf", SAMPLE_RATE * 8, 8 },
    { "sdrplay", SAMPLE_RATE * 4, 4 },
    { 0 }
};

static int find_supported_driver(const char *driver, float *samp_rate, int *decimation)
{
    for (unsigned int i = 0; supported_drivers[i].driver; i++)
    {
        if (strcasecmp(driver, supported_drivers[i].driver) == 0)
        {
            *samp_rate = supported_drivers[i].sample_rate;
            *decimation = supported_drivers[i].decimation;
            return 0;
        }
    }
    return 1;
}

static int snr_callback(void *arg, float snr, float pilot)
{
    nrsc5_t *st = arg;
    st->auto_gain_snr_ready = 1;
    if (pilot < AUTO_GAIN_MIN_PILOT)
        snr = 0.0f;
    st->auto_gain_snr = snr;
    return 1;
}

static float do_auto_gain(nrsc5_t *st, float step)
{
    float best_gain = 0, best_snr = 0;
    SoapySDRRange range;

    input_set_snr_callback(&st->input, snr_callback, st);

    range = SoapySDRDevice_getGainRange(st->dev, SOAPY_SDR_RX, RX_CHAN);
    for (float gain = range.minimum; gain < range.maximum + step - 0.1; gain += step)
    {
        int ignore;

        if (gain > range.maximum)
            gain = range.maximum;

        if (SoapySDRDevice_setGain(st->dev, SOAPY_SDR_RX, RX_CHAN, gain) != 0)
            continue;

        input_reset(&st->input);
        st->auto_gain_snr_ready = 0;

        // Two issues on RTL-SDR require ignoring the initial samples:
        //   - after changing the gain there are some samples queued with old gain
        //   - on Debian, changing the gain too quickly results in a freeze
        ignore = RX_TRANSITION_SAMPLES * st->decimation;
        while (!st->auto_gain_snr_ready)
        {
            int flags;
            long long timeNs;
            int count = SoapySDRDevice_readStream(st->dev, st->stream, (void**)&st->buffer,
                    RX_BUFFER_FFT * st->decimation, &flags, &timeNs, RX_TIMEOUT);
            if (count < 0)
                goto error;
            if (ignore >= count)
            {
                ignore -= count;
            }
            else
            {
                input_cb(st->buffer + ignore, count - ignore, &st->input);
                ignore = 0;
            }
        }
        // log_debug("AGC: %.2f (%.2f)", gain, st->auto_gain_snr);
        if (st->auto_gain_snr > best_snr)
        {
            best_snr = st->auto_gain_snr;
            best_gain = gain;
        }
        input_reset(&st->input);
    }

    log_debug("Gain: %.2f (%.2f)", best_gain, best_snr);
    st->gain = best_gain;

    SoapySDRDevice_setGain(st->dev, SOAPY_SDR_RX, RX_CHAN, st->gain);

    input_set_snr_callback(&st->input, NULL, NULL);
    return best_snr;

error:
    input_set_snr_callback(&st->input, NULL, NULL);
    return -1.0;
}

static void do_work(nrsc5_t *st)
{
    int count, flags;
    long long timeNs;

    if (st->stream)
    {
        count = SoapySDRDevice_readStream(st->dev, st->stream, (void **)&st->buffer,
                st->max_samples, &flags, &timeNs, RX_TIMEOUT);
        if (count > 0)
            input_cb(st->buffer, count, &st->input);
        st->samples += count;
    }
    else if (st->iq_file)
    {
        count = fread(st->buffer, sizeof(cint16_t), RX_BUFFER * st->decimation, st->iq_file);
        if (count > 0)
            input_cb(st->buffer, count, &st->input);
        else
            sleep(1);
    }
}

static void *worker_thread(void *arg)
{
    nrsc5_t *st = arg;

    pthread_mutex_lock(&st->worker_mutex);
    while (!st->closed)
    {
        if (st->stopped && !st->worker_stopped)
        {
            if (st->stream)
                SoapySDRDevice_deactivateStream(st->dev, st->stream, 0, 0);
            st->worker_stopped = 1;
            pthread_cond_broadcast(&st->worker_cond);
        }
        else if (!st->stopped && st->worker_stopped)
        {
            st->worker_stopped = 0;
            pthread_cond_broadcast(&st->worker_cond);

            if (st->stream)
            {
                if (SoapySDRDevice_activateStream(st->dev, st->stream, 0, 0, 0) != 0)
                    log_error("activate stream failed");

                if (st->auto_gain && st->gain < 0)
                {
                    if (do_auto_gain(st, AUTO_GAIN_STEP) < 0)
                    {
                        SoapySDRDevice_deactivateStream(st->dev, st->stream, 0, 0);
                        st->stopped = 1;
                        st->worker_stopped = 1;
                        pthread_cond_broadcast(&st->worker_cond);
                    }
                }
            }
        }

        if (st->stopped)
        {
            // wait for a signal
            pthread_cond_wait(&st->worker_cond, &st->worker_mutex);
        }
        else
        {
            pthread_mutex_unlock(&st->worker_mutex);
            do_work(st);
            pthread_mutex_lock(&st->worker_mutex);
        }
    }
    pthread_mutex_unlock(&st->worker_mutex);
    return NULL;
}

static void nrsc5_init(nrsc5_t *st)
{
    st->closed = 0;
    st->stopped = 1;
    st->worker_stopped = 1;
    st->auto_gain = 1;
    st->gain = -1;
    st->freq = NRSC5_SCAN_BEGIN;
    st->callback = NULL;

    output_init(&st->output, st);
    input_init(&st->input, st, &st->output);
    input_set_decimation(&st->input, st->decimation);

    st->max_samples = RX_BUFFER * st->decimation;
    st->buffer = malloc(st->max_samples * sizeof(cint16_t));

    // Create worker thread
    pthread_mutex_init(&st->worker_mutex, NULL);
    pthread_cond_init(&st->worker_cond, NULL);
    pthread_create(&st->worker, NULL, worker_thread, st);
}

int nrsc5_open(nrsc5_t **result, const char *args)
{
    const char *driver;
    size_t chan = RX_CHAN;
    float bw, samp_rate;
    nrsc5_t *st = calloc(1, sizeof(*st));

    st->dev = SoapySDRDevice_makeStrArgs(args);
    if (!st->dev)
        goto error_init;

    driver = SoapySDRDevice_getDriverKey(st->dev);
    log_info("Driver: %s", driver);
    log_info("Hardware: %s", SoapySDRDevice_getHardwareKey(st->dev));

    st->decimation = 2;
    samp_rate = SAMPLE_RATE * st->decimation;

    if (find_supported_driver(driver, &samp_rate, &st->decimation) != 0)
        log_warn("Unsupported driver (%s). Please report success or failure along with a debug log.", driver);

    if (SoapySDRDevice_setSampleRate(st->dev, SOAPY_SDR_RX, chan, samp_rate) != 0)
        goto error;
    SoapySDRDevice_setBandwidth(st->dev, SOAPY_SDR_RX, chan, samp_rate / 2);

    samp_rate = SoapySDRDevice_getSampleRate(st->dev, SOAPY_SDR_RX, chan);
    bw = SoapySDRDevice_getBandwidth(st->dev, SOAPY_SDR_RX, chan);

    log_info("Sample rate: %.2f", samp_rate);
    log_info("Bandwidth: %.2f", bw);
    log_debug("Decimation: %d", st->decimation);

    if (SoapySDRDevice_setGainMode(st->dev, SOAPY_SDR_RX, chan, 0) != 0)
        goto error;
    if (SoapySDRDevice_setGain(st->dev, SOAPY_SDR_RX, chan, st->gain) != 0)
        goto error;
    if (SoapySDRDevice_setFrequency(st->dev, SOAPY_SDR_RX, chan, st->freq + FREQ_OFFSET, NULL) != 0)
        goto error;
    if (SoapySDRDevice_setupStream(st->dev, &st->stream, SOAPY_SDR_RX, "CS16", &chan, 1, NULL) != 0)
        goto error;

    nrsc5_init(st);

    *result = st;
    return 0;

error:
    SoapySDRDevice_unmake(st->dev);
error_init:
    free(st);
    *result = NULL;
    return 1;
}

int nrsc5_open_iq(nrsc5_t **result, const char *path)
{
    FILE *fp;
    nrsc5_t *st;

    if (strcmp(path, "-") == 0)
        fp = stdin;
    else
        fp = fopen(path, "rb");
    if (fp == NULL)
        return 1;

    st = calloc(1, sizeof(*st));
    st->decimation = 2;
    st->iq_file = fp;
    nrsc5_init(st);
    input_set_offset_tuning(&st->input, 0);

    *result = st;
    return 0;
}

void nrsc5_close(nrsc5_t *st)
{
    if (!st)
        return;

    // signal the worker to exit
    pthread_mutex_lock(&st->worker_mutex);
    st->closed = 1;
    pthread_cond_broadcast(&st->worker_cond);
    pthread_mutex_unlock(&st->worker_mutex);

    // wait for worker to finish
    pthread_join(st->worker, NULL);

    if (st->stream)
        SoapySDRDevice_closeStream(st->dev, st->stream);
    if (st->dev)
        SoapySDRDevice_unmake(st->dev);
    if (st->iq_file)
        fclose(st->iq_file);
    if (st->buffer)
        free(st->buffer);
    free(st);
}

void nrsc5_start(nrsc5_t *st)
{
    if (st->scanning)
        return;

    // signal the worker to start
    pthread_mutex_lock(&st->worker_mutex);
    st->stopped = 0;
    pthread_cond_broadcast(&st->worker_cond);
    pthread_mutex_unlock(&st->worker_mutex);
}

void nrsc5_stop(nrsc5_t *st)
{
    if (st->scanning)
        return;

    // signal the worker to stop
    pthread_mutex_lock(&st->worker_mutex);
    st->stopped = 1;
    pthread_cond_broadcast(&st->worker_cond);
    pthread_mutex_unlock(&st->worker_mutex);

    // wait for worker to stop
    pthread_mutex_lock(&st->worker_mutex);
    while (st->stopped != st->worker_stopped)
        pthread_cond_wait(&st->worker_cond, &st->worker_mutex);
    pthread_mutex_unlock(&st->worker_mutex);
}

void nrsc5_get_frequency(nrsc5_t *st, float *freq)
{
    if (st->dev)
        *freq = SoapySDRDevice_getFrequency(st->dev, SOAPY_SDR_RX, RX_CHAN) - FREQ_OFFSET;
    else
        *freq = st->freq;
}

int nrsc5_set_frequency(nrsc5_t *st, float freq)
{
    if (st->freq == freq)
        return 0;
    if (!st->stopped)
        return 1;

    if (st->dev)
    {
        if (SoapySDRDevice_setFrequency(st->dev, SOAPY_SDR_RX, RX_CHAN, freq + FREQ_OFFSET, NULL) != 0)
            return 1;
        if (st->auto_gain)
            st->gain = -1;
        input_reset(&st->input);
        output_reset(&st->output);
    }

    st->freq = freq;
    return 0;
}

void nrsc5_get_gain(nrsc5_t *st, float *gain)
{
    if (st->dev)
        *gain = SoapySDRDevice_getGain(st->dev, SOAPY_SDR_RX, RX_CHAN);
    else
        *gain = st->gain;
}

int nrsc5_set_gain(nrsc5_t *st, float gain)
{
    if (st->gain == gain)
        return 0;
    if (!st->stopped)
        return 1;

    if (st->dev)
    {
        if (SoapySDRDevice_setGain(st->dev, SOAPY_SDR_RX, RX_CHAN, gain) != 0)
            return 1;
    }

    st->gain = gain;
    return 0;
}

void nrsc5_set_auto_gain(nrsc5_t *st, int enabled)
{
    st->auto_gain = enabled;
    st->gain = -1;
}

int nrsc5_scan(nrsc5_t *st, float begin, float end, float skip, float *result, const char **name)
{
    int ret = 1;

    if (!st->stopped)
            return 1;

    if (SoapySDRDevice_activateStream(st->dev, st->stream, 0, 0, 0) != 0)
        log_error("activate stream failed");

    st->scanning = 1;

    for (float freq = begin; ret && freq <= end; freq += skip)
    {
        float snr;

        if (nrsc5_set_frequency(st, freq) != 0)
            continue;
        *result = freq;

        snr = do_auto_gain(st, SCAN_AUTO_GAIN_STEP);
        if (snr == 0)
            continue;

        snr = do_auto_gain(st, AUTO_GAIN_STEP * 2);
        log_debug("Station @ %.1f (SNR %.2f)", freq, snr);
        if (snr < SCAN_MIN_SNR)
            continue;

        input_reset(&st->input);
        st->scan_name = NULL;
        st->scan_sync = 0;
        st->samples = 0;

        while (st->samples < SAMPLE_RATE * st->decimation * 30)
        {
            do_work(st);

            // Give up if we haven't found any signal for lock on
            if (!st->scan_sync && st->samples >= SAMPLE_RATE * st->decimation * 10)
                break;

            // Stop once we know the station name
            if (st->scan_name && st->scan_name[0])
                break;
        }

        if (!st->scan_sync)
            continue;

        if (st->scan_name)
            log_info("%s @ %.1f (SNR %.2f)", st->scan_name, freq, snr);

        *name = st->scan_name;
        ret = 0;
    }

    if (st->stream)
        SoapySDRDevice_deactivateStream(st->dev, st->stream, 0, 0);

    st->scanning = 0;
    return ret;
}

void nrsc5_set_callback(nrsc5_t *st, nrsc5_callback_t callback, void *opaque)
{
    pthread_mutex_lock(&st->worker_mutex);
    st->callback = callback;
    st->callback_opaque = opaque;
    pthread_mutex_unlock(&st->worker_mutex);
}

void nrsc5_report(nrsc5_t *st, const nrsc5_event_t *evt)
{
    if (st->scanning)
        return;

    if (st->callback)
        st->callback(evt, st->callback_opaque);
}

void nrsc5_report_iq(nrsc5_t *st, const void *data, size_t count)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_IQ;
    evt.iq.data = data;
    evt.iq.count = count;
    nrsc5_report(st, &evt);
}

void nrsc5_report_sync(nrsc5_t *st)
{
    nrsc5_event_t evt;

    if (st->scanning)
        st->scan_sync = 1;

    evt.event = NRSC5_EVENT_SYNC;
    nrsc5_report(st, &evt);
}

void nrsc5_report_lost_sync(nrsc5_t *st)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_LOST_SYNC;
    nrsc5_report(st, &evt);
}

void nrsc5_report_hdc(nrsc5_t *st, unsigned int program, const uint8_t *data, size_t count)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_HDC;
    evt.hdc.program = program;
    evt.hdc.data = data;
    evt.hdc.count = count;
    nrsc5_report(st, &evt);
}

void nrsc5_report_audio(nrsc5_t *st, unsigned int program, const int16_t *data, size_t count)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_AUDIO;
    evt.audio.program = program;
    evt.audio.data = data;
    evt.audio.count = count;
    nrsc5_report(st, &evt);
}

void nrsc5_report_mer(nrsc5_t *st, float lower, float upper)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_MER;
    evt.mer.lower = lower;
    evt.mer.upper = upper;
    nrsc5_report(st, &evt);
}

void nrsc5_report_ber(nrsc5_t *st, float cber)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_BER;
    evt.ber.cber = cber;
    nrsc5_report(st, &evt);
}

void nrsc5_report_lot(nrsc5_t *st, uint16_t port, unsigned int lot, unsigned int size, uint32_t mime, const char *name, const uint8_t *data)
{
    nrsc5_event_t evt;

    evt.event = NRSC5_EVENT_LOT;
    evt.lot.port = port;
    evt.lot.lot = lot;
    evt.lot.size = size;
    evt.lot.mime = mime;
    evt.lot.name = name;
    evt.lot.data = data;
    nrsc5_report(st, &evt);
}

static uint8_t convert_sig_component_type(uint8_t type)
{
    switch (type)
    {
    case SIG_COMPONENT_AUDIO: return NRSC5_SIG_COMPONENT_AUDIO;
    case SIG_COMPONENT_DATA: return NRSC5_SIG_COMPONENT_DATA;
    default:
        assert(0 && "Invalid component type");
        return 0;
    }
}

static uint8_t convert_sig_service_type(uint8_t type)
{
    switch (type)
    {
    case SIG_SERVICE_AUDIO: return NRSC5_SIG_SERVICE_AUDIO;
    case SIG_SERVICE_DATA: return NRSC5_SIG_SERVICE_DATA;
    default:
        assert(0 && "Invalid service type");
        return 0;
    }
}

void nrsc5_report_sig(nrsc5_t *st, sig_service_t *services, unsigned int count)
{
    nrsc5_event_t evt = { NRSC5_EVENT_SIG };
    nrsc5_sig_service_t *service = NULL;

    // convert internal structures to public structures
    for (unsigned int i = 0; i < count; i++)
    {
        nrsc5_sig_component_t *component = NULL;
        nrsc5_sig_service_t *prev = service;

        service = calloc(1, sizeof(nrsc5_sig_service_t));
        service->type = convert_sig_service_type(services[i].type);
        service->number = services[i].number;
        service->name = services[i].name;

        if (prev == NULL)
            evt.sig.services = service;
        else
            prev->next = service;

        for (unsigned int j = 0; j < MAX_SIG_COMPONENTS; j++)
        {
            nrsc5_sig_component_t *prevc = component;
            sig_component_t *internal = &services[i].component[j];

            if (internal->type == SIG_COMPONENT_NONE)
                continue;

            component = calloc(1, sizeof(nrsc5_sig_component_t));
            component->type = convert_sig_component_type(internal->type);
            component->id = internal->id;

            if (internal->type == SIG_COMPONENT_AUDIO)
            {
                component->audio.port = internal->audio.port;
                component->audio.type = internal->audio.type;
                component->audio.mime = internal->audio.mime;
            }
            else if (internal->type == SIG_COMPONENT_DATA)
            {
                component->data.port = internal->data.port;
                component->data.service_data_type = internal->data.service_data_type;
                component->data.type = internal->data.type;
                component->data.mime = internal->data.mime;
            }

            if (prevc == NULL)
                service->components = component;
            else
                prevc->next = component;
        }
    }

    nrsc5_report(st, &evt);

    // free the data structures
    for (service = evt.sig.services; service != NULL; )
    {
        void *p;
        nrsc5_sig_component_t *component;

        for (component = service->components; component != NULL; )
        {
            p = component;
            component = component->next;
            free(p);
        }

        p = service;
        service = service->next;
        free(p);
    }
}

void nrsc5_report_sis(nrsc5_t *st, pids_t *pids)
{
    nrsc5_event_t evt;

    if (st->scanning)
        st->scan_name = pids->short_name;

    evt.event = NRSC5_EVENT_SIS;
    evt.sis.name = pids->short_name;
    evt.sis.facility_id = pids->fcc_facility_id;

    nrsc5_report(st, &evt);
}
