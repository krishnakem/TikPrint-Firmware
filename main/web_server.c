/*
 * HTTP server.
 *
 * The UI (web/index.html) is gzipped at build time and embedded in the
 * firmware image, so it is served straight from flash with no file system.
 * The browser un-gzips it itself (Content-Encoding: gzip).
 */
#include "esp_log.h"
#include "esp_http_server.h"

#include "web_server.h"
#include "api.h"

static const char *TAG = "web";

// Symbols created by the build system for the embedded files (see main/CMakeLists.txt)
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t pdf_gz_start[]        asm("_binary_pdf_min_mjs_gz_start");
extern const uint8_t pdf_gz_end[]          asm("_binary_pdf_min_mjs_gz_end");
extern const uint8_t pdf_worker_gz_start[] asm("_binary_pdf_worker_min_mjs_gz_start");
extern const uint8_t pdf_worker_gz_end[]   asm("_binary_pdf_worker_min_mjs_gz_end");

static esp_err_t root_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET %s", req->uri);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // Always re-check with the board, so a re-flash shows the new UI immediately.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
}

/*
 * pdf.js: the library the page uses to draw PDFs (the board itself never reads a PDF).
 * The page only asks for these when someone drops a PDF. They never change for a given
 * version, and the page puts the version in the address (?v=...), so the browser may keep them.
 */
static esp_err_t send_script(httpd_req_t *req, const uint8_t *start, const uint8_t *end)
{
    ESP_LOGI(TAG, "GET %s", req->uri);
    httpd_resp_set_type(req, "text/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t pdf_get(httpd_req_t *req)
{
    return send_script(req, pdf_gz_start, pdf_gz_end);
}

static esp_err_t pdf_worker_get(httpd_req_t *req)
{
    return send_script(req, pdf_worker_gz_start, pdf_worker_gz_end);
}

/*
 * Fonts: the page draws the ticket's text with these (the board itself never draws text).
 * The page only downloads the one that is selected.
 */
#define FONT(uri_name, symbol) \
    extern const uint8_t symbol##_start[] asm("_binary_" #symbol "_woff2_start"); \
    extern const uint8_t symbol##_end[]   asm("_binary_" #symbol "_woff2_end");
FONT("inter", inter)
FONT("source-serif", source_serif)
FONT("jetbrains-mono", jetbrains_mono)
FONT("caveat", caveat)
FONT("nunito", nunito)
#undef FONT

typedef struct {
    const char *uri;
    const uint8_t *start, *end;
} font_file_t;

static const font_file_t FONT_FILES[] = {
    { "/fonts/inter.woff2",          inter_start,          inter_end },
    { "/fonts/source-serif.woff2",   source_serif_start,   source_serif_end },
    { "/fonts/jetbrains-mono.woff2", jetbrains_mono_start, jetbrains_mono_end },
    { "/fonts/caveat.woff2",         caveat_start,         caveat_end },
    { "/fonts/nunito.woff2",         nunito_start,         nunito_end },
};

static esp_err_t font_get(httpd_req_t *req)
{
    const font_file_t *f = req->user_ctx;
    ESP_LOGI(TAG, "GET %s", req->uri);
    httpd_resp_set_type(req, "font/woff2");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    return httpd_resp_send(req, (const char *)f->start, f->end - f->start);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;  // browsers open several connections; recycle the oldest
    config.max_uri_handlers = 32;
    config.stack_size = 8192;        // Wi-Fi scan + JSON building need more than the default

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_register_uri_handler(server, &root);
    const httpd_uri_t pdf = { .uri = "/vendor/pdf.min.mjs", .method = HTTP_GET, .handler = pdf_get };
    httpd_register_uri_handler(server, &pdf);
    const httpd_uri_t pdf_worker = { .uri = "/vendor/pdf.worker.min.mjs", .method = HTTP_GET, .handler = pdf_worker_get };
    httpd_register_uri_handler(server, &pdf_worker);
    for (size_t i = 0; i < sizeof(FONT_FILES) / sizeof(FONT_FILES[0]); i++) {
        const httpd_uri_t font = { .uri = FONT_FILES[i].uri, .method = HTTP_GET, .handler = font_get,
                                   .user_ctx = (void *)&FONT_FILES[i] };
        httpd_register_uri_handler(server, &font);
    }
    api_register(server);

    ESP_LOGI(TAG, "HTTP server listening on port %d (UI is %u bytes gzipped)", config.server_port,
             (unsigned)(index_html_gz_end - index_html_gz_start));
    return ESP_OK;
}
