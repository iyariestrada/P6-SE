#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "ships.h"

#define MAX_SCORE 3
#define RESTART_BUTTON GPIO_NUM_19
#define DEBOUNCER_DELAY_uS 4000
#define BOARD_SIZE 10

static uint8_t score = 0;
static uint8_t turn_num = 30;
static ship *ships = NULL;

static int64_t last_interrupt_time = 0;
static uint8_t bounce_rectifier = 0xFF;

static const char *TAG = "web_server";

int game_restarted = 0;

static const char display_score[] = "<h1>Score: %d | Turnos: %d </h1>";
static const char table_init[] = "<table><tr><td></td><td>0</td><td>1</td><td>2</td><td>3</td><td>4</td><td>5</td><td>6</td><td>7</td><td>8</td><td>9</td></tr>";
static const char table_end[] = "</table></body></html>";
static const char table_row[] = "  <tr><td>%c</td><td>%c</td><td>%c</td><td>%c</td><td>%c</td><td>%c</td><td>%c</td><td>%c</td><td>%c</td><td>%c</td><td>%c</td></tr>";

static char board_empty[BOARD_SIZE][BOARD_SIZE] = {{"??????????"}, {"??????????"}, {"??????????"}, {"??????????"}, {"??????????"}, {"??????????"}, {"??????????"}, {"??????????"}, {"??????????"}, {"??????????"}};
static char ships_on_board[BOARD_SIZE][BOARD_SIZE];

void update_board(uint8_t x_coor, uint8_t y_coor)
{
    if (x_coor < BOARD_SIZE && y_coor < BOARD_SIZE)
    {
        if (ships_on_board[x_coor][y_coor] == 1 && board_empty[x_coor][y_coor] != 'X')
        {
            board_empty[x_coor][y_coor] = 'X';
            score++;
        }
        else if (board_empty[x_coor][y_coor] == '?')
        {
            board_empty[x_coor][y_coor] = ' ';
            turn_num--;
        }
    }
}

void restart_game(void)
{
    score = 0;
    turn_num = 30;
    get_board_filled(ships_on_board, ships, get_ships(&ships));

    for (uint8_t row = 0; row < BOARD_SIZE; row++)
    {
        for (uint8_t col = 0; col < BOARD_SIZE; col++)
        {
            board_empty[row][col] = '?';
        }
    }
    game_restarted = 0;
}

static void IRAM_ATTR gpio_interrupt_handler(void *args)
{
    int pinNumber = (int)args;
    int64_t current_time = esp_timer_get_time();
    if ((current_time - last_interrupt_time) >= DEBOUNCER_DELAY_uS)
    {
        last_interrupt_time = current_time;
        bounce_rectifier++;
        if (bounce_rectifier & 1)
            game_restarted = 1;
    }
}

void init_spiffs()
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Error al iniciar psiffs (%s)", esp_err_to_name(ret));
        return;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Error al obtener informacion de spiffs (%s)", esp_err_to_name(ret));
        return;
    }
    else
    {
        ESP_LOGI(TAG, "Total: %d, Usado: %d", total, used);
    }
}

esp_err_t html_get_handler(httpd_req_t *req)
{
    if (game_restarted)
    {
        const char *resp = "<html><head><meta http-equiv=\"refresh\" content=\"3; url=/\"></head><body><h1>Reiniciando Juego... </h1></body></html>";
        httpd_resp_sendstr(req, resp);
        restart_game();
        game_restarted = 0;
        return ESP_OK;
    }
    if (turn_num == 0)
    {
        const char *resp = "<html><head><meta http-equiv=\"refresh\" content=\"3; url=/\"></head><body><h1>Game Over</h1></body></html>";
        httpd_resp_sendstr(req, resp);
        restart_game();
        return ESP_OK;
    }
    else if (score >= MAX_SCORE)
    {
        const char *resp = "<html><head><meta http-equiv=\"refresh\" content=\"3; url=/\"></head><body><h1>You Win</h1></body></html>";
        httpd_resp_sendstr(req, resp);
        restart_game();
        return ESP_OK;
    }
    else
    {
        FILE *file = fopen("/spiffs/index.html", "r");

        if (file == NULL)
        {
            ESP_LOGE(TAG, "Error al abrir el archivo");
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }

        char *line = (char *)malloc(256);

        while (fgets(line, sizeof(line), file))
        {
            httpd_resp_sendstr_chunk(req, line);
        }

        asprintf(&line, display_score, score, turn_num);
        httpd_resp_sendstr_chunk(req, line);

        // Enviar la tabla del juego
        httpd_resp_sendstr_chunk(req, table_init);

        for (int cnt = 0; cnt < 10; cnt++)
        {
            asprintf(&line, table_row, (char)(cnt + '0'), board_empty[cnt][0], board_empty[cnt][1], board_empty[cnt][2], board_empty[cnt][3],
                     board_empty[cnt][4], board_empty[cnt][5], board_empty[cnt][6], board_empty[cnt][7], board_empty[cnt][8], board_empty[cnt][9]);
            httpd_resp_sendstr_chunk(req, line);
        }

        httpd_resp_sendstr_chunk(req, table_end);
        httpd_resp_sendstr_chunk(req, NULL);
        free(line);
        fclose(file);
    }

    return ESP_OK;
}

esp_err_t html_post_handler(httpd_req_t *req)
{

    ESP_LOGI(TAG, "POST request");

    uint8_t inputs[req->content_len];
    ESP_LOGI(TAG, "Content length: %d", req->content_len);

    httpd_req_recv(req, (char *)inputs, req->content_len);
    inputs[req->content_len] = '\0';

    ESP_LOGI(TAG, "Datos recibidos: %s", inputs);

    update_board(inputs[6] - '0', inputs[2] - '0');

    html_get_handler(req);

    return ESP_OK;
}

esp_err_t css_get_handler(httpd_req_t *req)
{
    FILE *file = fopen("/spiffs/style.css", "r");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Error al abrir el archivo");
        httpd_resp_send_404(req);

        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/css");

    char line[256];

    while (fgets(line, sizeof(line), file))
    {
        httpd_resp_sendstr_chunk(req, line);
    }

    httpd_resp_sendstr_chunk(req, NULL);

    fclose(file);

    return ESP_OK;
}
void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t conf = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&conf));

    const char mi_ssid[] = "ferrax's_esp";

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = sizeof(mi_ssid),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK}};

    memcpy(wifi_config.ap.ssid, mi_ssid, sizeof(mi_ssid));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID: %s password: %s", wifi_config.ap.ssid, wifi_config.ap.password);
}

void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t html = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = html_get_handler,
            .user_ctx = NULL};

        httpd_uri_t css = {
            .uri = "/style.css",
            .method = HTTP_GET,
            .handler = css_get_handler,
            .user_ctx = NULL};

        httpd_uri_t html_post = {
            .uri = "/",
            .method = HTTP_POST,
            .handler = html_post_handler,
            .user_ctx = NULL};

        httpd_register_uri_handler(server, &html_post);
        httpd_register_uri_handler(server, &html);
        httpd_register_uri_handler(server, &css);
    }
    else
    {
        ESP_LOGE(TAG, "Error al iniciar el servidor");
    }
}

void app_main()
{
    char aux[11];

    uint8_t ships_num = get_ships(&ships);

    get_board_filled(ships_on_board, ships, ships_num);

    for (uint8_t row = 0; row < 10; row++)
    {
        for (uint8_t col = 0; col < 10; col++)
        {
            aux[col] = ships_on_board[row][col] + '0';
        }
        ESP_LOGI(TAG, "%s", aux);
    }

    free(ships);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    init_spiffs();
    wifi_init_softap();

    // Configurar GPIO
    gpio_reset_pin(RESTART_BUTTON);
    gpio_set_direction(RESTART_BUTTON, GPIO_MODE_INPUT);

    gpio_set_intr_type(RESTART_BUTTON, GPIO_INTR_NEGEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(RESTART_BUTTON, gpio_interrupt_handler, (void *)RESTART_BUTTON);

    start_webserver();
}