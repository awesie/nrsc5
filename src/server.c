#include <cjson/cJSON.h>
#include <libwebsockets.h>
#include <nrsc5.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>

#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
    #include <sys/syscall.h> /* SYS_gettid */
#elif defined(__WINNT__)
    #include <direct.h> /* _mkdir */
    #include <windows.h> /* GetCurrentThreadId */
    #define mkdir(x,y) _mkdir(x)
#endif

#define AUDIO_FRAME_SAMPLES 2048
#define MAX_RADIO_PROGRAMS 8
// Pages are ~4KB, so the total buffer is ~128KB
#define BUFFER_SIZE 32
#define MAX_POST_SIZE 1024

typedef struct server_id3_t {
    struct server_id3_t *next;
    uint64_t granule;

    char *title;
    char *artist;
    char *album;
    char *genre;
    char *ufid_owner;
    char *ufid_id;
    int xhdr_param;
    uint32_t xhdr_mime;
    int xhdr_lot;
} server_id3_t;

typedef struct {
    int id;

    ogg_stream_state os;
    vorbis_info vi;
    vorbis_dsp_state vd;
    vorbis_block vb;

    uint8_t *header;
    size_t header_len;

    int page_idx;
    int page_len;
    struct {
        uint64_t granule;
        uint8_t *data;
        size_t len;
    } page[BUFFER_SIZE];

    unsigned int hdc_packets;
    unsigned int hdc_bytes;
    float hdc_bitrate;

    server_id3_t *id3_head, *id3_tail;

    uint16_t port_station_logo;
    uint16_t port_primary_image;
} server_program_t;

typedef struct scan_result_t {
    struct scan_result_t *next;
    float frequency;
    char *name;
} scan_result_t;

typedef struct {
    struct lws_context *context;
    nrsc5_t *radio;
    pthread_mutex_t mutex;
    pthread_mutex_t lws_mutex;
    unsigned int generation;
    char *cache_path;
    char *static_path;

    server_program_t *program[8];
    unsigned int facility_id;
    char *name;

    int sync;
    float frequency;
    float cber;
    float mer_lower;
    float mer_upper;

    int scanning;
    scan_result_t *scan_result;
} server_t;

enum
{
    SESSION_INVALID = 0,
    SESSION_STREAM,
    SESSION_JSON
};

typedef struct {
    int type;
    union
    {
        struct
        {
            unsigned int generation;
            int program;
            int sent_header;
            int page_idx;
        } stream;
        struct
        {
            int post;
            char *post_uri;
            char *post_buf;
            size_t post_idx;
            char *response;
        } json;
    };
} session_data_t;

static int http_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static void server_reset(server_t *server);
static int server_set_frequency(server_t *server, float frequency);
static int server_start_scan(server_t *server);

static int force_exit = 0;
static const struct lws_protocols protocols[] = {
    { "http-only", http_callback, sizeof(session_data_t), 0 },
    { NULL, NULL, 0, 0 }
};

static int get_thread_id()
{
#if defined(__APPLE__)
    return pthread_mach_thread_np(pthread_self());
#elif defined(__linux__)
    return syscall(SYS_gettid);
#elif defined(__WINNT__)
    return GetCurrentThreadId();
#else
    #error "Unsupported platform"
#endif
}

static const char *mime_to_ext(uint32_t mime)
{
    if (mime == NRSC5_MIME_JPEG)
        return "jpg";
    else if (mime == NRSC5_MIME_PNG)
        return "png";
    else
        return NULL;
}

static const char *mime_to_string(uint32_t mime)
{
    if (mime == NRSC5_MIME_PRIMARY_IMAGE)
        return "primary-image";
    else if (mime == NRSC5_MIME_STATION_LOGO)
        return "station-logo";
    else if (mime == NRSC5_MIME_TEXT)
        return "text";
    else if (mime == NRSC5_MIME_JPEG)
        return "jpeg";
    else if (mime == NRSC5_MIME_PNG)
        return "png";
    else
        return "";
}

static server_program_t *server_program_create(int id)
{
    ogg_packet header, header_comm, header_code;
    ogg_page og;
    vorbis_comment vc;
    server_program_t *sp = calloc(1, sizeof(server_program_t));

    sp->id = id;

    vorbis_info_init(&sp->vi);
    vorbis_comment_init(&vc);
    vorbis_encode_init_vbr(&sp->vi, 2, 44100, 0.4);
    vorbis_analysis_init(&sp->vd, &sp->vi);
    vorbis_block_init(&sp->vd, &sp->vb);

    ogg_stream_init(&sp->os, 0);

    vorbis_analysis_headerout(&sp->vd, &vc, &header, &header_comm, &header_code);
    ogg_stream_packetin(&sp->os, &header);
    ogg_stream_packetin(&sp->os, &header_comm);
    ogg_stream_packetin(&sp->os, &header_code);

    sp->header = malloc(10);
    sp->header_len = 10;
    while (1)
    {
        size_t len;
        int result = ogg_stream_flush(&sp->os, &og);
        if (result == 0)
            break;
        len = sp->header_len + og.header_len + og.body_len;
        sp->header = realloc(sp->header, len);
        memcpy(sp->header + sp->header_len, og.header, og.header_len);
        sp->header_len += og.header_len;
        memcpy(sp->header + sp->header_len, og.body, og.body_len);
        sp->header_len += og.body_len;
    }
    sp->header = realloc(sp->header, sp->header_len + 2);
    sprintf((char *)sp->header, "%08lX", (long)sp->header_len - 10);
    sp->header[8] = '\r';
    sp->header[9] = '\n';
    sp->header[sp->header_len++] = '\r';
    sp->header[sp->header_len++] = '\n';

    return sp;
}

static void server_id3_destroy(server_id3_t *id3)
{
    free(id3->title);
    free(id3->artist);
    free(id3->album);
    free(id3->genre);
    free(id3->ufid_owner);
    free(id3->ufid_id);
    free(id3);
}

static void server_program_destroy(server_program_t *sp)
{
    server_id3_t *id3;

    if (sp == NULL)
        return;

    ogg_stream_clear(&sp->os);
    vorbis_info_clear(&sp->vi);
    vorbis_dsp_clear(&sp->vd);
    vorbis_block_clear(&sp->vb);

    free(sp->header);

    for (int i = 0; i < sp->page_len; i++)
        free(sp->page[i].data);

    id3 = sp->id3_head;
    while (id3 != NULL)
    {
        server_id3_t *next = id3->next;
        server_id3_destroy(id3);
        id3 = next;
    }

    free(sp);
}

static int updated_id3_string(char *old, const char *value)
{
    if (old && value && strcmp(old, value) == 0)
        return 0;
    if (old == NULL && value == NULL)
        return 0;

    return 1;
}

#define COPY_STRING(x,y) x = y ? strdup(y) : NULL
static int server_program_update_id3(server_t *server, server_program_t *sp, const nrsc5_event_t *evt)
{
    int updated = 1;
    server_id3_t *id3 = sp->id3_tail;

    if (id3)
    {
        updated = 0;
        updated |= updated_id3_string(id3->title, evt->id3.title);
        updated |= updated_id3_string(id3->artist, evt->id3.artist);
        updated |= updated_id3_string(id3->album, evt->id3.album);
        updated |= updated_id3_string(id3->genre, evt->id3.genre);
        updated |= updated_id3_string(id3->ufid_owner, evt->id3.ufid.owner);
        updated |= updated_id3_string(id3->ufid_id, evt->id3.ufid.id);
        updated |= id3->xhdr_param != evt->id3.xhdr.param;
        updated |= id3->xhdr_mime != evt->id3.xhdr.mime;
        updated |= id3->xhdr_lot != evt->id3.xhdr.lot;
    }

    if (updated)
    {
        id3 = calloc(1, sizeof(server_id3_t));
        id3->granule = sp->vd.granulepos;
        COPY_STRING(id3->title, evt->id3.title);
        COPY_STRING(id3->artist, evt->id3.artist);
        COPY_STRING(id3->album, evt->id3.album);
        COPY_STRING(id3->genre, evt->id3.genre);
        COPY_STRING(id3->ufid_owner, evt->id3.ufid.owner);
        COPY_STRING(id3->ufid_id, evt->id3.ufid.id);
        id3->xhdr_param = evt->id3.xhdr.param;
        id3->xhdr_mime = evt->id3.xhdr.mime;
        id3->xhdr_lot = evt->id3.xhdr.lot;

        if (sp->id3_tail)
            sp->id3_tail->next = id3;
        else
            sp->id3_head = id3;
        sp->id3_tail = id3;
    }

    return updated;
}

static void server_program_expire_id3(server_program_t *sp, uint64_t granule)
{
    while (sp->id3_head != sp->id3_tail && sp->id3_head->next->granule < granule)
    {
        server_id3_t *next = sp->id3_head->next;
        server_id3_destroy(sp->id3_head);
        sp->id3_head = next;
    }
}

static void server_program_push_page(server_t *server, server_program_t *sp, ogg_page *og)
{
    uint8_t *data;
    size_t len = 0;

    data = malloc(32 + og->header_len + og->body_len);
    len += sprintf((char *)data, "%lX\r\n", og->header_len + og->body_len);
    memcpy(data + len, og->header, og->header_len);
    len += og->header_len;
    memcpy(data + len, og->body, og->body_len);
    len += og->body_len;
    data[len++] = '\r';
    data[len++] = '\n';

    if (sp->page_len == BUFFER_SIZE)
    {
        server_program_expire_id3(sp, sp->page[0].granule);

        free(sp->page[0].data);
        memmove(&sp->page[0], &sp->page[1], sizeof(sp->page[0]) * (BUFFER_SIZE - 1));
        sp->page_idx++;
        sp->page_len--;
    }
    sp->page[sp->page_len].granule = sp->vd.granulepos;
    sp->page[sp->page_len].data = data;
    sp->page[sp->page_len].len = len;
    sp->page_len++;
}

static int server_program_push(server_t *server, server_program_t *sp, const int16_t *samples, size_t count)
{
    int should_notify = 0;
    float **anabuf;
    ogg_packet op;
    ogg_page og;

    anabuf = vorbis_analysis_buffer(&sp->vd, count / 2);
    for (size_t i = 0; i < count / 2; i++)
    {
        anabuf[0][i] = samples[i * 2] / 32768.0f;
        anabuf[1][i] = samples[i * 2 + 1] / 32768.0f;
    }
    vorbis_analysis_wrote(&sp->vd, count / 2);

    while (vorbis_analysis_blockout(&sp->vd, &sp->vb) == 1)
    {
        vorbis_analysis(&sp->vb, NULL);
        vorbis_bitrate_addblock(&sp->vb);

        while (vorbis_bitrate_flushpacket(&sp->vd, &op))
        {
            ogg_stream_packetin(&sp->os, &op);

            while (1)
            {
                if (ogg_stream_pageout(&sp->os, &og) == 0)
                    break;
                server_program_push_page(server, sp, &og);
                should_notify = 1;
            }
        }
    }

    return should_notify;
}

int lws_header_table_detach(struct lws *wsi, int autoservice);
void lws_header_table_force_to_detachable_state(struct lws *wsi);
static int handle_stream_request(server_t *server, session_data_t *session, struct lws *wsi, const char *url)
{
    int program = -1;
    unsigned int generation;
    uint8_t buffer[4096], *p, *end;

    if (!lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI))
    {
        lws_return_http_status(wsi, HTTP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return -1;
    }

    if (strcasecmp(url, "/stream_0.ogg") == 0)
        program = 0;
    else if (strcasecmp(url, "/stream_1.ogg") == 0)
        program = 1;
    else if (strcasecmp(url, "/stream_2.ogg") == 0)
        program = 2;
    else if (strcasecmp(url, "/stream_3.ogg") == 0)
        program = 3;
    else if (strcasecmp(url, "/stream_4.ogg") == 0)
        program = 4;
    else if (strcasecmp(url, "/stream_5.ogg") == 0)
        program = 5;
    else if (strcasecmp(url, "/stream_6.ogg") == 0)
        program = 6;
    else if (strcasecmp(url, "/stream_7.ogg") == 0)
        program = 7;

    pthread_mutex_lock(&server->mutex);
    if (program != -1 && server->program[program] == NULL)
        program = -1;
    generation = server->generation;
    pthread_mutex_unlock(&server->mutex);

    if (program == -1)
    {
        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
        return -1;
    }

    session->type = SESSION_STREAM;
    session->stream.generation = generation;
    session->stream.program = program;
    session->stream.sent_header = 0;
    session->stream.page_idx = 0;

    if (lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_RANGE))
    {
        unsigned int range_start;

        if (lws_hdr_copy(wsi, (char *)buffer, sizeof(buffer), WSI_TOKEN_HTTP_RANGE) < 0)
            return -1;
        if (sscanf((char *)buffer, "bytes=%u-", &range_start) != 1)
            return -1;

        if (range_start > 0)
        {
            lws_return_http_status(wsi, HTTP_STATUS_REQ_RANGE_NOT_SATISFIABLE, NULL);
            return -1;
        }
    }

    p = buffer;
    end = buffer + sizeof(buffer);

    if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
        return -1;
    if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, (uint8_t *)"audio/ogg", 9, &p, end))
        return -1;
    if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_TRANSFER_ENCODING, (uint8_t *)"chunked", 7, &p, end))
        return -1;
    if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_ACCEPT_RANGES, (uint8_t *)"none", 4, &p, end))
        return -1;
    if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CACHE_CONTROL, (uint8_t *)"no-cache, no-store, must-revalidate, max-age=0", 47, &p, end))
        return -1;
    if (lws_finalize_http_header(wsi, &p, end))
        return -1;

    if (lws_write(wsi, buffer, p - buffer, LWS_WRITE_HTTP_HEADERS) < 0)
        return -1;

    // We need to detach wsi->ah, otherwise libwebsockets is unhappy about the long-lived connection.
    // N.B. these are private functions and should not be used by us.
    lws_header_table_force_to_detachable_state(wsi);
    lws_header_table_detach(wsi, 0);

    lws_callback_on_writable(wsi);
    return 0;
}

static int handle_stream_writeable(server_t *server, session_data_t *session, struct lws *wsi)
{
    int result = 0;
    server_program_t *sp;

    pthread_mutex_lock(&server->mutex);
    if (server->generation != session->stream.generation)
        goto error;

    sp = server->program[session->stream.program];
    if (session->stream.sent_header)
    {
        int page_idx = session->stream.page_idx;
        if (page_idx == 0)
            page_idx = sp->page_idx;
        if (page_idx < sp->page_idx)
            goto error;
        if (page_idx < sp->page_idx + sp->page_len)
        {
            if (lws_write(wsi, sp->page[page_idx - sp->page_idx].data, sp->page[page_idx - sp->page_idx].len, LWS_WRITE_HTTP) < 0)
                goto error;
            lws_set_timeout(wsi, PENDING_TIMEOUT_HTTP_CONTENT, 5);
            session->stream.page_idx = page_idx + 1;
        }

        if (session->stream.page_idx < sp->page_idx + sp->page_len)
            lws_callback_on_writable(wsi);
    }
    else
    {
        if (lws_write(wsi, sp->header, sp->header_len, LWS_WRITE_HTTP) < 0)
            goto error;

        session->stream.sent_header = 1;
        lws_callback_on_writable(wsi);
    }

done:
    pthread_mutex_unlock(&server->mutex);
    return result;

error:
    result = 1;
    goto done;
}

static int handle_json_request_post(server_t *server, session_data_t *session, struct lws *wsi)
{
    cJSON *data, *resp = NULL;
    uint8_t tmp[1024], *p, *end;

    session->json.post_buf[session->json.post_idx] = 0;
    data = cJSON_Parse(session->json.post_buf);
    if (data == NULL)
        goto bad_request;

    if (strcasecmp(session->json.post_uri, "/api/frequency") == 0)
    {
        int success = 1;
        float new_frequency;
        const cJSON *req_frequency = cJSON_GetObjectItemCaseSensitive(data, "frequency");
        if (!cJSON_IsNumber(req_frequency))
            goto bad_request;
        new_frequency = req_frequency->valuedouble;

        resp = cJSON_CreateObject();

        if (new_frequency != server->frequency)
            success = server_set_frequency(server, new_frequency);

        cJSON_AddItemToObject(resp, "success", cJSON_CreateBool(success));
        cJSON_AddItemToObject(resp, "frequency", cJSON_CreateNumber(server->frequency));
    }
    else if (strcasecmp(session->json.post_uri, "/api/scan") == 0)
    {
        int success;

        success = server_start_scan(server);
        resp = cJSON_CreateObject();
        cJSON_AddItemToObject(resp, "success", cJSON_CreateBool(success));
    }
    else
    {
        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
        goto error;
    }

    if (resp)
        session->json.response = cJSON_PrintUnformatted(resp);

    p = tmp;
    end = tmp + sizeof(tmp);

    if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
        goto error;
    if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, (uint8_t *)"application/json", 16, &p, end))
        goto error;
    if (session->json.response && lws_add_http_header_content_length(wsi, strlen(session->json.response), &p, end))
        goto error;
    if (lws_finalize_http_header(wsi, &p, end))
        goto error;

    if (lws_write(wsi, tmp, p - tmp, LWS_WRITE_HTTP_HEADERS) < 0)
        goto error;

    cJSON_Delete(data);
    cJSON_Delete(resp);

    if (session->json.response)
    {
        lws_callback_on_writable(wsi);
        return 0;
    }
    else
    {
        return -1;
    }

bad_request:
    lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
error:
    cJSON_Delete(data);
    cJSON_Delete(resp);
    return -1;
}

static void get_program_image(server_t *server, server_program_t *sp, server_id3_t *id3, char *buf, size_t buflen)
{
    char tmp[1024], filename[32];

    if (server->facility_id == 0)
        goto error;

    if (id3->xhdr_param == 0 && id3->xhdr_mime == NRSC5_MIME_PRIMARY_IMAGE && id3->xhdr_lot != -1)
    {
        sprintf(filename, "%d-%d-%d.png", server->facility_id, sp->id, id3->xhdr_lot);
        if (snprintf(tmp, sizeof(tmp), "%s/%s", server->cache_path, filename) >= sizeof(tmp))
            goto error;
        if (access(tmp, F_OK) == 0 && snprintf(buf, buflen, "/cache/%s", filename) < buflen)
            return;

        sprintf(filename, "%d-%d-%d.jpg", server->facility_id, sp->id, id3->xhdr_lot);
        if (snprintf(tmp, sizeof(tmp), "%s/%s", server->cache_path, filename) >= sizeof(tmp))
            goto error;
        if (access(tmp, F_OK) == 0 && snprintf(buf, buflen, "/cache/%s", filename) < buflen)
            return;

        // failed, try station logo instead
    }

    sprintf(filename, "%d-%d-logo.png", server->facility_id, sp->id);
    if (snprintf(tmp, sizeof(tmp), "%s/%s", server->cache_path, filename) >= sizeof(tmp))
        goto error;
    if (access(tmp, F_OK) == 0 && snprintf(buf, buflen, "/cache/%s", filename) < buflen)
        return;

    sprintf(filename, "%d-%d-logo.jpg", server->facility_id, sp->id);
    if (snprintf(tmp, sizeof(tmp), "%s/%s", server->cache_path, filename) >= sizeof(tmp))
        goto error;
    if (access(tmp, F_OK) == 0 && snprintf(buf, buflen, "/cache/%s", filename) < buflen)
        return;

error:
    buf[0] = 0;
}

static cJSON *server_id3_to_json(server_t *server, server_program_t *sp, server_id3_t *id3)
{
    char path[256];
    cJSON *resp = cJSON_CreateObject();

    cJSON_AddItemToObject(resp, "timestamp", cJSON_CreateNumber((double)id3->granule / 44100.0));
    if (id3->title)
        cJSON_AddItemToObject(resp, "title", cJSON_CreateString(id3->title));
    if (id3->artist)
        cJSON_AddItemToObject(resp, "artist", cJSON_CreateString(id3->artist));
    if (id3->album)
        cJSON_AddItemToObject(resp, "album", cJSON_CreateString(id3->album));
    if (id3->genre)
        cJSON_AddItemToObject(resp, "genre", cJSON_CreateString(id3->genre));
    if (id3->ufid_owner)
        cJSON_AddItemToObject(resp, "ufid_owner", cJSON_CreateString(id3->ufid_owner));
    if (id3->ufid_id)
        cJSON_AddItemToObject(resp, "ufid_id", cJSON_CreateString(id3->ufid_id));
    cJSON_AddItemToObject(resp, "xhdr_param", cJSON_CreateNumber(id3->xhdr_param));
    cJSON_AddItemToObject(resp, "xhdr_mime", cJSON_CreateStringReference(mime_to_string(id3->xhdr_mime)));
    cJSON_AddItemToObject(resp, "xhdr_lot", cJSON_CreateNumber(id3->xhdr_lot));
    get_program_image(server, sp, id3, path, sizeof(path));
    cJSON_AddItemToObject(resp, "image", cJSON_CreateString(path));

    return resp;
}

static int handle_json_request(server_t *server, session_data_t *session, struct lws *wsi, const char *url)
{
    uint8_t tmp[1024], *p, *end;
    cJSON *resp = NULL;

    session->type = SESSION_JSON;

    if (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI))
    {
        // FIXME hangs if content-length is 0
        session->json.post = 1;
        session->json.post_uri = strdup(url);
        return 0;
    }
    else if (!lws_hdr_total_length(wsi, WSI_TOKEN_GET_URI))
    {
        lws_return_http_status(wsi, HTTP_STATUS_METHOD_NOT_ALLOWED, NULL);
        return -1;
    }

    if (strcasecmp(url, "/api/status") == 0)
    {
        cJSON *resp_programs;

        pthread_mutex_lock(&server->mutex);
        resp = cJSON_CreateObject();
        if (server->name)
            cJSON_AddItemToObject(resp, "name", cJSON_CreateString(server->name));
        if (server->facility_id)
            cJSON_AddItemToObject(resp, "facility_id", cJSON_CreateNumber(server->facility_id));
        cJSON_AddItemToObject(resp, "sync", cJSON_CreateBool(server->sync));
        cJSON_AddItemToObject(resp, "frequency", cJSON_CreateNumber(server->frequency));
        cJSON_AddItemToObject(resp, "cber", cJSON_CreateNumber(server->cber));
        cJSON_AddItemToObject(resp, "mer_lower", cJSON_CreateNumber(server->mer_lower));
        cJSON_AddItemToObject(resp, "mer_upper", cJSON_CreateNumber(server->mer_upper));
        cJSON_AddItemToObject(resp, "scanning", cJSON_CreateBool(server->scanning));

        resp_programs = cJSON_CreateArray();
        for (unsigned int i = 0; i < MAX_RADIO_PROGRAMS; i++)
        {
            char path[256];
            cJSON *resp_program, *resp_id3;
            server_program_t *sp = server->program[i];
            if (sp == NULL)
                continue;

            resp_id3 = cJSON_CreateArray();
            for (server_id3_t *id3 = sp->id3_head; id3 != NULL; id3 = id3->next)
                cJSON_AddItemToArray(resp_id3, server_id3_to_json(server, sp, id3));

            resp_program = cJSON_CreateObject();
            cJSON_AddItemToObject(resp_program, "id", cJSON_CreateNumber(i));
            cJSON_AddItemToObject(resp_program, "hdc_packets", cJSON_CreateNumber(sp->hdc_packets));
            cJSON_AddItemToObject(resp_program, "hdc_bytes", cJSON_CreateNumber(sp->hdc_bytes));
            cJSON_AddItemToObject(resp_program, "hdc_bitrate", cJSON_CreateNumber(sp->hdc_bitrate));
            cJSON_AddItemToObject(resp_program, "id3", resp_id3);
            sprintf(path, "/stream_%d.ogg", i);
            cJSON_AddItemToObject(resp_program, "audio", cJSON_CreateString(path));

            cJSON_AddItemToArray(resp_programs, resp_program);
        }
        cJSON_AddItemToObject(resp, "programs", resp_programs);

        pthread_mutex_unlock(&server->mutex);
    }
    else if (strcasecmp(url, "/api/scan") == 0)
    {
        cJSON *resp_results;

        pthread_mutex_lock(&server->mutex);
        resp = cJSON_CreateObject();
        cJSON_AddItemToObject(resp, "scanning", cJSON_CreateBool(server->scanning));
        if (server->scanning)
            cJSON_AddItemToObject(resp, "frequency", cJSON_CreateNumber(server->frequency));

        resp_results = cJSON_CreateArray();
        for (scan_result_t *scan = server->scan_result; scan != NULL; scan = scan->next)
        {
            cJSON *resp_scan = cJSON_CreateObject();
            cJSON_AddItemToObject(resp_scan, "frequency", cJSON_CreateNumber(scan->frequency));
            cJSON_AddItemToObject(resp_scan, "name", cJSON_CreateString(scan->name));
            cJSON_AddItemToArray(resp_results, resp_scan);
        }

        cJSON_AddItemToObject(resp, "results", resp_results);
        pthread_mutex_unlock(&server->mutex);
    }
    else
    {
        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
        return -1;
    }

    if (resp)
        session->json.response = cJSON_PrintUnformatted(resp);

    cJSON_Delete(resp);

    p = tmp;
    end = tmp + sizeof(tmp);

    if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
        return -1;
    if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, (uint8_t *)"application/json", 16, &p, end))
        return -1;
    if (session->json.response && lws_add_http_header_content_length(wsi, strlen(session->json.response), &p, end))
        return -1;
    if (lws_finalize_http_header(wsi, &p, end))
        return -1;

    if (lws_write(wsi, tmp, p - tmp, LWS_WRITE_HTTP_HEADERS) < 0)
        return -1;

    if (session->json.response)
    {
        lws_callback_on_writable(wsi);
        return 0;
    }
    else
    {
        return -1;
    }
}

static int handle_json_writeable(server_t *server, session_data_t *session, struct lws *wsi)
{
    if (session->json.response)
        lws_write(wsi, (uint8_t *)session->json.response, strlen(session->json.response), LWS_WRITE_HTTP);
    return -1;
}

static int handle_cache_request(server_t *server, session_data_t *session, struct lws *wsi, const char *url)
{
    char tmp[1024];
    const char *content_type, *filename, *urlext;

    filename = url + 7;
    if (filename[strspn(filename, "abcdefghijklmnopqrstuvwxyz0123456789-_.")] != 0)
        goto not_found;

    if (snprintf(tmp, sizeof(tmp), "%s/%s", server->cache_path, filename) >= sizeof(tmp))
    {
        lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
        return -1;
    }

    if (access(tmp, F_OK) != 0)
        goto not_found;

    urlext = filename + strlen(filename) - 4;
    if (urlext < filename)
        content_type = NULL;
    else if (strcmp(urlext, ".jpg") == 0)
        content_type = "image/jpeg";
    else if (strcmp(urlext, ".png") == 0)
        content_type = "image/png";
    else
        content_type = NULL;

    if (content_type == NULL)
        goto not_found;

    if (lws_serve_http_file(wsi, tmp, content_type, NULL, 0) < 0)
        return -1;

    return 0;

not_found:
    lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
    return -1;
}

static int handle_static_request(server_t *server, session_data_t *session, struct lws *wsi, const char *url)
{
    char tmp[1024];
    const char *content_type, *urlext;

    if (url[0] != '/')
        goto not_found;

    if (url[1] == 0)
        url = "/index.html";

    if (url[strspn(url, "abcdefghijklmnopqrstuvwxyz0123456789-_./")] != 0)
        goto not_found;

    if (strstr(url, "..") != NULL)
        goto not_found;

    if (snprintf(tmp, sizeof(tmp), "%s%s", server->static_path, url) >= sizeof(tmp))
    {
        lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
        return -1;
    }

    if (access(tmp, F_OK) != 0)
        goto not_found;

    urlext = url + strlen(url) - 4;
    if (urlext < url)
        content_type = NULL;
    else if (strcmp(urlext, ".jpg") == 0)
        content_type = "image/jpeg";
    else if (strcmp(urlext, ".png") == 0)
        content_type = "image/png";
    else if (strcmp(urlext, ".txt") == 0)
        content_type = "text/plain";
    else if (strcmp(urlext, ".css") == 0)
        content_type = "text/css";
    else if (strcmp(urlext - 1, ".html") == 0)
        content_type = "text/html";
    else if (strcmp(urlext + 1, ".js") == 0)
        content_type = "text/javascript";
    else
        content_type = "application/octet-stream";

    if (content_type == NULL)
        goto not_found;

    if (lws_serve_http_file(wsi, tmp, content_type, NULL, 0) < 0)
        return -1;

    return 0;

not_found:
    lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
    return -1;
}

static int http_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    struct lws_context *context = lws_get_context(wsi);
    server_t *server = lws_context_user(context);
    session_data_t *session = user;

    switch (reason)
    {
    case LWS_CALLBACK_HTTP:
        if (len < 1)
        {
            lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
            return -1;
        }

        if (strncasecmp(in, "/stream", 7) == 0)
            return handle_stream_request(server, session, wsi, in);
        else if (strncasecmp(in, "/api", 4) == 0)
            return handle_json_request(server, session, wsi, in);
        else if (strncasecmp(in, "/cache/", 7) == 0)
            return handle_cache_request(server, session, wsi, in);
        else
            return handle_static_request(server, session, wsi, in);

        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
        return -1;
    case LWS_CALLBACK_HTTP_BODY:
        if (session->type != SESSION_JSON || !session->json.post)
            return -1;
        if (session->json.post_idx + len > MAX_POST_SIZE)
        {
            lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
            return -1;
        }
        if (!session->json.post_buf)
            session->json.post_buf = malloc(MAX_POST_SIZE + 1);
        memcpy(session->json.post_buf + session->json.post_idx, in, len);
        session->json.post_idx += len;
        return 0;
    case LWS_CALLBACK_HTTP_BODY_COMPLETION:
        if (session->type != SESSION_JSON || !session->json.post)
            return -1;
        return handle_json_request_post(server, session, wsi);
    case LWS_CALLBACK_HTTP_WRITEABLE:
        switch (session->type)
        {
        case SESSION_STREAM:
            return handle_stream_writeable(server, session, wsi);
        case SESSION_JSON:
            return handle_json_writeable(server, session, wsi);
        }
        return -1;
    case LWS_CALLBACK_HTTP_FILE_COMPLETION:
        return -1;
    case LWS_CALLBACK_HTTP_DROP_PROTOCOL:
        switch (session->type)
        {
        case SESSION_JSON:
            free(session->json.post_uri);
            session->json.post_uri = NULL;
            free(session->json.post_buf);
            session->json.post_buf = NULL;
            free(session->json.response);
            session->json.response = NULL;
            break;
        }
        session->type = SESSION_INVALID;
        return 0;
    case LWS_CALLBACK_LOCK_POLL:
        if (len)
            pthread_mutex_lock(&server->lws_mutex);
        return 0;
    case LWS_CALLBACK_UNLOCK_POLL:
        if (len)
            pthread_mutex_unlock(&server->lws_mutex);
        return 0;
    case LWS_CALLBACK_GET_THREAD_ID:
        return get_thread_id();
    default:
        return 0;
    }
}

static server_program_t *ensure_server_program(server_t *server, int program)
{
    server_program_t *sp;

    sp = server->program[program];
    if (sp == NULL)
        server->program[program] = sp = server_program_create(program);
    return sp;
}

static void save_primary_image(server_t *server, server_program_t *sp, unsigned int lot, const uint8_t *data, unsigned int size, uint32_t mime)
{
    char path[1024];
    FILE *fp;
    const char *ext = mime_to_ext(mime);

    if (ext == NULL || server->facility_id == 0)
        return;

    if (snprintf(path, sizeof(path), "%s/%d-%d-%d.%s", server->cache_path, server->facility_id, sp->id, lot, ext) >= sizeof(path))
        return;

    fp = fopen(path, "wb");
    fwrite(data, 1, size, fp);
    fclose(fp);
}

static void save_station_logo(server_t *server, server_program_t *sp, const uint8_t *data, unsigned int size, uint32_t mime)
{
    char path[1024];
    FILE *fp;
    const char *ext = mime_to_ext(mime);

    if (ext == NULL || server->facility_id == 0)
        return;

    if (snprintf(path, sizeof(path), "%s/%d-%d-logo.%s", server->cache_path, server->facility_id, sp->id, ext) >= sizeof(path))
        return;

    fp = fopen(path, "wb");
    fwrite(data, 1, size, fp);
    fclose(fp);
}

static void radio_callback(const nrsc5_event_t *evt, void *opaque)
{
    int should_notify = 0;
    server_program_t *sp;
    server_t *server = opaque;

    pthread_mutex_lock(&server->mutex);

    switch (evt->event)
    {
    case NRSC5_EVENT_BER:
        server->cber = evt->ber.cber;
        break;
    case NRSC5_EVENT_MER:
        server->mer_lower = evt->mer.lower;
        server->mer_upper = evt->mer.upper;
        break;
    case NRSC5_EVENT_HDC:
        sp = ensure_server_program(server, evt->hdc.program);
        sp->hdc_packets++;
        sp->hdc_bytes += evt->hdc.count * sizeof(evt->hdc.data[0]);
        sp->hdc_bitrate = (float)sp->hdc_bytes * 8 * 44100 / 2048 / sp->hdc_packets / 1000;
        break;
    case NRSC5_EVENT_AUDIO:
        sp = ensure_server_program(server, evt->audio.program);
        should_notify = server_program_push(server, sp, evt->audio.data, evt->audio.count);
        break;
    case NRSC5_EVENT_SYNC:
        server->sync = 1;
        break;
    case NRSC5_EVENT_LOST_SYNC:
        server->sync = 0;
        break;
    case NRSC5_EVENT_ID3:
        sp = ensure_server_program(server, evt->id3.program);
        server_program_update_id3(server, sp, evt);
        // TODO
        break;
    case NRSC5_EVENT_SIG:
        for (nrsc5_sig_service_t *service = evt->sig.services; service != NULL; service = service->next)
        {
            int program = -1;
            uint16_t port_primary_image = 0, port_station_logo = 0;

            if (service->type != NRSC5_SIG_SERVICE_AUDIO)
                continue;

            for (nrsc5_sig_component_t *comp = service->components; comp != NULL; comp = comp->next)
            {
                if (comp->type == NRSC5_SIG_COMPONENT_AUDIO)
                {
                    program = comp->audio.port;
                }
                else if (comp->type == NRSC5_SIG_COMPONENT_DATA)
                {
                    if (comp->data.mime == NRSC5_MIME_PRIMARY_IMAGE)
                        port_primary_image = comp->data.port;
                    else if (comp->data.mime == NRSC5_MIME_STATION_LOGO)
                        port_station_logo = comp->data.port;
                }
            }

            if (program != -1)
            {
                sp = ensure_server_program(server, program);
                sp->port_primary_image = port_primary_image;
                sp->port_station_logo = port_station_logo;
            }
        }
        break;
    case NRSC5_EVENT_LOT:
        for (unsigned int i = 0; i < MAX_RADIO_PROGRAMS; i++)
        {
            server_program_t *sp = server->program[i];
            if (sp == NULL)
                continue;
            if (evt->lot.port == sp->port_primary_image)
            {
                save_primary_image(server, sp, evt->lot.lot, evt->lot.data, evt->lot.size, evt->lot.mime);
                break;
            }
            else if (evt->lot.port == sp->port_station_logo)
            {
                save_station_logo(server, sp, evt->lot.data, evt->lot.size, evt->lot.mime);
                break;
            }
        }
        break;
    case NRSC5_EVENT_SIS:
        server->facility_id = evt->sis.facility_id;
        if (server->name == NULL || strcmp(server->name, evt->sis.name) != 0)
        {
            free(server->name);
            server->name = strdup(evt->sis.name);
        }
        break;
    }

    pthread_mutex_unlock(&server->mutex);

    if (should_notify)
    {
        pthread_mutex_lock(&server->lws_mutex);
        lws_callback_on_writable_all_protocol(server->context, &protocols[0]);
        pthread_mutex_unlock(&server->lws_mutex);
    }
}

static void server_reset_scan(server_t *server)
{
    while (server->scan_result)
    {
        scan_result_t *next = server->scan_result->next;
        free(server->scan_result->name);
        free(server->scan_result);
        server->scan_result = next;
    }
}

// This should be called any time we are changing stations.
static void server_reset(server_t *server)
{
    pthread_mutex_lock(&server->mutex);
    // invalidate streams
    server->generation++;

    // free program information
    for (int i = 0; i < MAX_RADIO_PROGRAMS; i++)
    {
        server_program_destroy(server->program[i]);
        server->program[i] = NULL;
    }

    // free station information
    free(server->name);
    server->name = NULL;
    server->facility_id = 0;
    server->sync = 0;
    server->cber = 0;
    server->mer_lower = 0;
    server->mer_upper = 0;
    pthread_mutex_unlock(&server->mutex);

    pthread_mutex_lock(&server->lws_mutex);
    lws_callback_on_writable_all_protocol(server->context, &protocols[0]);
    pthread_mutex_unlock(&server->lws_mutex);
}

static int server_set_frequency(server_t *server, float frequency)
{
    int success = 0;

    if (!server->scanning)
    {
        nrsc5_stop(server->radio);
        success = nrsc5_set_frequency(server->radio, frequency) == 0;
        if (success)
            server_reset(server);
        nrsc5_get_frequency(server->radio, &server->frequency);
        nrsc5_start(server->radio);
    }
    return success;
}

static void *server_scan_worker(void *arg)
{
    const char *name;
    server_t *server = arg;
    scan_result_t *tail = NULL;

    server->frequency = NRSC5_SCAN_BEGIN;
    while (nrsc5_scan(server->radio, server->frequency, NRSC5_SCAN_END, NRSC5_SCAN_SKIP, &server->frequency, &name) == 0)
    {
        scan_result_t *result = malloc(sizeof(scan_result_t));
        result->next = NULL;
        result->frequency = server->frequency;
        result->name = name ? strdup(name) : NULL;

        pthread_mutex_lock(&server->mutex);
        if (tail)
            tail->next = result;
        else
            server->scan_result = result;
        tail = result;
        pthread_mutex_unlock(&server->mutex);

        server->frequency += NRSC5_SCAN_SKIP;
    }

    server->scanning = 0;
    if (server->scan_result)
        server_set_frequency(server, server->scan_result->frequency);
    return NULL;
}

static int server_start_scan(server_t *server)
{
    int success = 0;

    if (!server->scanning)
    {
        pthread_t worker;

        server->scanning = 1;
        nrsc5_stop(server->radio);
        server_reset(server);
        server_reset_scan(server);
        pthread_create(&worker, NULL, server_scan_worker, server);
        success = 1;
    }

    return success;
}

static void sigint_handler(int sig)
{
    force_exit = 1;
}

int main(int argc, char *argv[])
{
    int n = 0;
    struct lws_context_creation_info info = { 0 };
    server_t *server = calloc(1, sizeof(server_t));

    server->cache_path = "cache";
    mkdir(server->cache_path, 0700);
    server->static_path = "static";

    pthread_mutex_init(&server->mutex, NULL);
    pthread_mutex_init(&server->lws_mutex, NULL);
    if (nrsc5_open(&server->radio, argv[1]) != 0)
    {
        lwsl_err("Open device failed.\n");
        return 1;
    }
    nrsc5_set_callback(server->radio, radio_callback, server);

    info.port = 8888;
    info.protocols = protocols;
    info.user = server;

    server->context = lws_create_context(&info);
    if (server->context == NULL)
    {
        lwsl_err("libwebsocket init failed\n");
        return 1;
    }

    signal(SIGINT, sigint_handler);

    server_set_frequency(server, 96.7e6);
    while (n >= 0 && !force_exit)
    {
        n = lws_service(server->context, 100);
    }

    lws_context_destroy(server->context);
    lwsl_notice("Good-bye.\n");
    return 0;
}
