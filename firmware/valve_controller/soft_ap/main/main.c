// main.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "esp_http_server.h"
#include "driver/gpio.h"
#include "cJSON.h"

// lwIP helpers
#include "lwip/inet.h"      // ipaddr_addr (o inet_aton)
#include "lwip/ip_addr.h"

static const char *TAG = "ap_http_gpio";

/* ======= CONFIGURABLES ======= */
static const char *AP_SSID    = "MiESP_AP";      // Cambiá acá
static const char *AP_PASS    = "miclave123";    // >= 8 chars para WPA2
static const int   AP_CHANNEL = 6;               // 1/6/11 recomendado
//static const int   MAX_CONN   = 4;
static const char *AP_IP      = "192.168.4.1";   // IP fija de la interfaz AP
static const char *AP_NETMASK = "255.255.255.0";
static const int   SERVER_PORT = 8080;           // Puerto HTTP

/* ======= UTIL: setear IP fija en softAP ======= */
static esp_err_t set_softap_ip(const char *ip_str, const char *netmask_str)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        ESP_LOGE(TAG, "No se encontró WIFI_AP_DEF");
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr      = ipaddr_addr(ip_str);
    ip_info.gw.addr      = ipaddr_addr(ip_str);     // gateway = propia IP
    ip_info.netmask.addr = ipaddr_addr(netmask_str);

    // DHCP server debe pararse para cambiar IP
    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps_stop: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

    // Volver a iniciar DHCP server si lo querés activo (recomendado en AP)
    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "dhcps_start: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "IP fija seteada: %s", ip_str);
    return ESP_OK;
}

/* ======= HTTP handler: POST /gpio ======= */
static esp_err_t gpio_post_handler(httpd_req_t *req)
{
    // Leer body completo
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body size invalid");
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    int recvd = 0;
    while (recvd < total) {
        int r = httpd_req_recv(req, buf + recvd, total - recvd);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        recvd += r;
    }
    buf[recvd] = '\0';

    // Parsear JSON
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_FAIL;
    }

    cJSON *j_gpio = cJSON_GetObjectItemCaseSensitive(root, "GPIO");
    cJSON *j_sts  = cJSON_GetObjectItemCaseSensitive(root, "sts");

    if (!cJSON_IsNumber(j_gpio) || !cJSON_IsString(j_sts)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid fields");
        return ESP_FAIL;
    }

    int gpio_num = j_gpio->valueint;
    const char *sts = j_sts->valuestring;

    if (gpio_num < 0 || gpio_num > 46) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "GPIO out of range");
        return ESP_FAIL;
    }

    // Configurar pin como salida
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    // sts: "on" -> 1; cualquier otro -> 0
    int level = (strcasecmp(sts, "on") == 0) ? 1 : 0;
    gpio_set_level(gpio_num, level);

    cJSON_Delete(root);

    // Respuesta OK
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"result\":\"ok\"}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ======= Iniciar servidor HTTP ======= */
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SERVER_PORT;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar HTTP server");
        return NULL;
    }

    httpd_uri_t uri_gpio = {
        .uri = "/gpio",
        .method = HTTP_POST,
        .handler = gpio_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_gpio);

    ESP_LOGI(TAG, "HTTP server iniciado en puerto %d", SERVER_PORT);
    return server;
}

/* ======= Inicializar SoftAP (orden robusto) ======= */
static void wifi_init_softap(void)
{
    // 1) Crear netif default AP ANTES de init WiFi
    esp_netif_create_default_wifi_ap();

    // 2) Init WiFi driver
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    wifi_country_t country = { .cc="AR", .schan=1, .nchan=13, .policy=WIFI_COUNTRY_POLICY_MANUAL };
ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    wifi_config_t ap_cfg = {0};
    strlcpy((char*)ap_cfg.ap.ssid, "MiESP_AP", sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len        = 0;                   // DEJAR 0 -> el driver calcula
    ap_cfg.ap.ssid_hidden     = 0;
    ap_cfg.ap.channel         = AP_CHANNEL;                   // probá 6, si no 11
    ap_cfg.ap.max_connection  = 4;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.authmode        = WIFI_AUTH_WPA_WPA2_PSK;
    strlcpy((char*)ap_cfg.ap.password, "miclave123", sizeof(ap_cfg.ap.password));

    // Para test extremo: red abierta (sólo para descartar WPA)
    if (true) {                                    // poné true para test
        ap_cfg.ap.password[0] = '\0';
        ap_cfg.ap.authmode    = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    // Protocolos “legacy + n” (mejor descubrimiento)
    ESP_ERROR_CHECK(esp_wifi_set_protocol(
        WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));

    // Sin power-save, 20 MHz, potencia alta
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(78);

    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    // 5) Potencia TX (opcional)
    esp_wifi_set_max_tx_power(78); // ~máx

    // 6) Arrancar WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    // 7) Setear IP fija en el AP
    ESP_ERROR_CHECK(set_softap_ip(AP_IP, AP_NETMASK));

    // 8) Log de verificación de lo que quedó
    wifi_config_t check = {0};
    esp_wifi_get_config(WIFI_IF_AP, &check);
    ESP_LOGI(TAG, "SoftAP listo. SSID:'%s' len:%d hidden:%d auth:%d ch:%d",
             check.ap.ssid, check.ap.ssid_len, check.ap.ssid_hidden,
             check.ap.authmode, check.ap.channel);
}

/* ======= app_main ======= */
void app_main(void)
{
    // NVS requerido por WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Stack de red + loop de eventos
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // WiFi AP + servidor
    wifi_init_softap();
    start_webserver();

    wifi_config_t check = {0};
    esp_wifi_get_config(WIFI_IF_AP, &check);
    uint8_t proto = 0;
    esp_wifi_get_protocol(WIFI_IF_AP, &proto);
    ESP_LOGI(TAG, "AP-> SSID:'%s' ch:%d auth:%d hidden:%d beacon:%u proto:0x%02X",
            check.ap.ssid, check.ap.channel, check.ap.authmode,
            check.ap.ssid_hidden, check.ap.beacon_interval, proto);
    ESP_LOGI(TAG, "Sistema inicializado.");
}
