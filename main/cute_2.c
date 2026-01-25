
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"


#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "portmacro.h"
#include "protocol_examples_common.h"
#include "esp_netif.h"

#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#ifdef CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
#include "psa/crypto.h"
#endif
#include "esp_crt_bundle.h"






static const char *TAG = "example";



/* Constants that aren't configurable in menuconfig */
#define WEB_SERVER "feeds.libsyn.com"
#define WEB_PORT "443"
#define WEB_URL "https://feeds.libsyn.com/130245/rss"



static const char *REQUEST = "GET " WEB_URL " HTTP/1.0\r\n"
    "Host: "WEB_SERVER"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";


// http://feeds.libsyn.com/130245/rss

// https://feeds.npr.org/510333/podcast.xml



static QueueHandle_t payload_queue;




// static int https_fetch_stuff(FILE* output)
static void https_fetch_stuff(void* arg)
{
    char buf[0xff];
    int ret, flags, len;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt cacert;
    mbedtls_ssl_config conf;
    mbedtls_net_context server_fd;

#ifdef CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize PSA crypto, returned %d", (int) status);
        // return;
        vTaskDelete(NULL);
    }
#endif

    mbedtls_ssl_init(&ssl);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    ESP_LOGI(TAG, "Seeding the random number generator");

    mbedtls_ssl_config_init(&conf);

    mbedtls_entropy_init(&entropy);
    if((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed returned %d", ret);
        // abort();
        // return 1;

        vTaskDelete(NULL);

    }

    ESP_LOGI(TAG, "Attaching the certificate bundle...");

    ret = esp_crt_bundle_attach(&conf);

    if(ret < 0)
    {
        ESP_LOGE(TAG, "esp_crt_bundle_attach returned -0x%x", -ret);
        // abort();
        // return 1;
        vTaskDelete(NULL);

    }

    ESP_LOGI(TAG, "Setting hostname for TLS session...");

     /* Hostname set here should match CN in server certificate */
    if((ret = mbedtls_ssl_set_hostname(&ssl, WEB_SERVER)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_set_hostname returned -0x%x", -ret);
        // abort();
        // return 1;
        vTaskDelete(NULL);

    }

    ESP_LOGI(TAG, "Setting up the SSL/TLS structure...");

    if((ret = mbedtls_ssl_config_defaults(&conf,
                                          MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_config_defaults returned %d", ret);
        goto exit;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
#ifdef CONFIG_MBEDTLS_DEBUG
    mbedtls_esp_enable_debug_log(&conf, CONFIG_MBEDTLS_DEBUG_LEVEL);
#endif

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0)
    {
        ESP_LOGE(TAG, "mbedtls_ssl_setup returned -0x%x", -ret);
        goto exit;
    }




    // while(1) {


        mbedtls_net_init(&server_fd);

        ESP_LOGI(TAG, "Connecting to %s:%s...", WEB_SERVER, WEB_PORT);

        if ((ret = mbedtls_net_connect(&server_fd, WEB_SERVER,
                                      WEB_PORT, MBEDTLS_NET_PROTO_TCP)) != 0)
        {
            ESP_LOGE(TAG, "mbedtls_net_connect returned -%x", -ret);
            goto exit;
        }

        ESP_LOGI(TAG, "Connected.");

        mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

        ESP_LOGI(TAG, "Performing the SSL/TLS handshake...");

        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
        {
            ESP_LOGE(TAG, "ERROR HANDSHAKE");

            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                ESP_LOGE(TAG, "mbedtls_ssl_handshake returned -0x%x", -ret);
                goto exit;
            }
        }

        ESP_LOGI(TAG, "Verifying peer X.509 certificate...");

        if ((flags = mbedtls_ssl_get_verify_result(&ssl)) != 0)
        {
            /* In real life, we probably want to close connection if ret != 0 */
            ESP_LOGW(TAG, "Failed to verify peer certificate!");
            bzero(buf, sizeof(buf));
            mbedtls_x509_crt_verify_info(buf, sizeof(buf), "  ! ", flags);
            ESP_LOGW(TAG, "verification info: %s", buf);
        }
        else {
            ESP_LOGI(TAG, "Certificate verified.");
        }

        ESP_LOGI(TAG, "Cipher suite is %s", mbedtls_ssl_get_ciphersuite(&ssl));

        ESP_LOGI(TAG, "Writing HTTP request...");

        size_t written_bytes = 0;
        do {
            ret = mbedtls_ssl_write(&ssl,
                                    (const unsigned char *)REQUEST + written_bytes,
                                    strlen(REQUEST) - written_bytes);
            if (ret >= 0) {
                ESP_LOGI(TAG, "%d bytes written", ret);
                written_bytes += ret;
            } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_WANT_READ) {
                ESP_LOGE(TAG, "mbedtls_ssl_write returned -0x%x", -ret);
                goto exit;
            }
        } while(written_bytes < strlen(REQUEST));

        ESP_LOGI(TAG, "Reading HTTP response...");

        do
        {
            len = sizeof(buf) - 1;
            bzero(buf, sizeof(buf));

            // ugh?
            ret = mbedtls_ssl_read(&ssl, (unsigned char *)buf, len);

#if CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 && CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS
            if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                ESP_LOGD(TAG, "got session ticket in TLS 1.3 connection, retry read");
                continue;
            }
#endif // CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 && CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS

            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }

            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                ret = 0;
                break;
            }

            if (ret < 0) {
                // -0x4c
                ESP_LOGE(TAG, "mbedtls_ssl_read returned -0x%x", -ret);
                break;
            }

            if (ret == 0) {
                ESP_LOGI(TAG, "connection closed");
                break;
            }

            len = ret;
            ESP_LOGI(TAG, "%d bytes read", len);
            /* Print response directly to stdout as it is read */
            // for (int i = 0; i < len; i++) {
            //     putchar(buf[i]);
            // }

            // if (fwrite(buf, 1, len, output) < len) {
            //     ESP_LOGD(TAG, "file written to max size? file write error");
            //     // ?
            //     break;
            // }

            // queue is the same size


            for (int i = 0; i < len; i++) {
                // putchar(buf[i]);


                if (xQueueSend(payload_queue, buf + i, portMAX_DELAY) != pdPASS) {

                    ESP_LOGE(TAG, "send to queue failed...");
                    goto exit;

                }



            }

            // ESP_LOGI(TAG, "wrote to output file");



        } while(1);

        mbedtls_ssl_close_notify(&ssl);

    exit:
        mbedtls_ssl_session_reset(&ssl);
        mbedtls_net_free(&server_fd);

        if (ret != 0) {
            mbedtls_strerror(ret, buf, 100);
            ESP_LOGE(TAG, "Last error was: -0x%x - %s", -ret, buf);
        
            // return 1;
        
            vTaskDelete(NULL);

        }

        vTaskDelete(NULL);

        // return 0;


        // putchar('\n'); // JSON output doesn't have a newline at end

        // static int request_count;
        // ESP_LOGI(TAG, "Completed %d requests", ++request_count);
        // printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());


        // for (int countdown = 10; countdown >= 0; countdown--) {
        //     ESP_LOGI(TAG, "%d...", countdown);
        //     vTaskDelay(1000 / portTICK_PERIOD_MS);
        // }
        // ESP_LOGI(TAG, "Starting again!");
    // }
}



static void write_stuff(void* arg) 
{
    // how to exit?

    while (1) {
        char c;

        if (xQueueReceive(payload_queue, &c, portMAX_DELAY)) {
            putchar(c);



        }
    }

}


void app_main(void)
{

    // printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());



    // hm
    payload_queue = xQueueCreate(0xff, sizeof(char));



    // check if file exists, 
    // if not, fetch it, store it
    // read it out  ?




//     ESP_LOGI(TAG, "Initializing SPIFFS");

//     esp_vfs_spiffs_conf_t conf = {
//       .base_path = "/spiffs",
//       .partition_label = NULL,
//       .max_files = 5,
//       .format_if_mount_failed = true
//     };

//     // Use settings defined above to initialize and mount SPIFFS filesystem.
//     // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
//     esp_err_t ret = esp_vfs_spiffs_register(&conf);

//     if (ret != ESP_OK) {
//         if (ret == ESP_FAIL) {
//             ESP_LOGE(TAG, "Failed to mount or format filesystem");
//         } else if (ret == ESP_ERR_NOT_FOUND) {
//             ESP_LOGE(TAG, "Failed to find SPIFFS partition");
//         } else {
//             ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
//         }
//         return;
//     }

// // #ifdef CONFIG_EXAMPLE_SPIFFS_CHECK_ON_START
//     ESP_LOGI(TAG, "Performing SPIFFS_check().");
//     ret = esp_spiffs_check(conf.partition_label);
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
//         return;
//     } else {
//         ESP_LOGI(TAG, "SPIFFS_check() successful");
//     }
// // #endif


//     size_t total = 0, used = 0;
//     ret = esp_spiffs_info(conf.partition_label, &total, &used);
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
//         esp_spiffs_format(conf.partition_label);
//         return;
//     } else {
//         ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
//     }

//     // Check consistency of reported partition size info.
//     if (used > total) {
//         ESP_LOGW(TAG, "Number of used bytes cannot be larger than total. Performing SPIFFS_check().");
//         ret = esp_spiffs_check(conf.partition_label);
//         // Could be also used to mend broken files, to clean unreferenced pages, etc.
//         // More info at https://github.com/pellepl/spiffs/wiki/FAQ#powerlosses-contd-when-should-i-run-spiffs_check
//         if (ret != ESP_OK) {
//             ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
//             return;
//         } else {
//             ESP_LOGI(TAG, "SPIFFS_check() successful");
//         }
//     }



    // ESP_LOGI(TAG, "trying Opening file");
    // FILE* f;

    // struct stat st;
    // unsigned char buf[0xff];
    // int read;

    // if (stat("/spiffs/hello.txt", &st) == 0) {
    //     f = fopen("/spiffs/hello.txt", "r");

    //     if (f == NULL) {
    //         return;
    //     }

    //     while ((read = fread(buf, 1, 0xff, f)) != 0) {
    //         fwrite(buf, 1, read, stdout);
    //     }
    //     fputc('\n', stdout);

    //     ESP_LOGI(TAG, "Closing file, leaving");
    //     fclose(f);

    //     // return;
    // }





    // Use POSIX and C standard library functions to work with files.
    // First create a file.

    // ESP_LOGI(TAG, "Opening file");
    // f = fopen("/spiffs/hello.txt", "w");
    // if (f == NULL) {
    //     ESP_LOGE(TAG, "Failed to open file for writing");
    //     return;
    // }


    // fprintf(f, "Hello World!\n");
    // fclose(f);
    // ESP_LOGI(TAG, "File written");


    // startup network stuff
    // don't return after this point?
    // 

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());


    // if (https_fetch_stuff(f)) {
        // ESP_LOGE(TAG, "Failed to store https content");
    // }



    // fclose(f);



    // prio 3

    // needs big stacks
    xTaskCreatePinnedToCore(https_fetch_stuff, "https_fetch_stuff", 40000, NULL, 3, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(write_stuff, "write_stuff", 4096, NULL, 3, NULL, tskNO_AFFINITY);

    vTaskDelay(pdMS_TO_TICKS(20000));
    ESP_LOGI(TAG, "after delay main");


    // vTaskSuspend(TaskHandle_t xTaskToSuspend)

    
    // ESP_LOGI(TAG, "Attempting to read stored content");

    // struct stat st;
    // unsigned char buf[0xff];
    // int read;

    // if (stat("/spiffs/hello.txt", &st) == 0) {
    //     f = fopen("/spiffs/hello.txt", "r");

    //     if (f == NULL) {
    //         ESP_LOGE(TAG, "fail");
    //         return;
    //     }

    //     while ((read = fread(buf, 1, 0xff, f)) != 0) {
    //         fwrite(buf, 1, read, stdout);
    //     }
    //     fputc('\n', stdout);

    //     ESP_LOGI(TAG, "Closing file, leaving");
    //     fclose(f);

    //     // return;
    // }





    // // Check if destination file exists before renaming
    // struct stat st;
    // if (stat("/spiffs/foo.txt", &st) == 0) {
    //     // Delete it if it exists
    //     unlink("/spiffs/foo.txt");
    // }

    // // Rename original file
    // ESP_LOGI(TAG, "Renaming file");
    // if (rename("/spiffs/hello.txt", "/spiffs/foo.txt") != 0) {
    //     ESP_LOGE(TAG, "Rename failed");
    //     return;
    // }

    // // Open renamed file for reading
    // ESP_LOGI(TAG, "Reading file");
    // f = fopen("/spiffs/foo.txt", "r");
    // if (f == NULL) {
    //     ESP_LOGE(TAG, "Failed to open file for reading");
    //     return;
    // }
    // char line[64];
    // fgets(line, sizeof(line), f);
    // fclose(f);
    // // strip newline
    // char* pos = strchr(line, '\n');
    // if (pos) {
    //     *pos = '\0';
    // }
    // ESP_LOGI(TAG, "Read from file: '%s'", line);


    // end net
    ESP_ERROR_CHECK(example_disconnect());



    // All done, unmount partition and disable SPIFFS
    // esp_vfs_spiffs_unregister(conf.partition_label);
    // ESP_LOGI(TAG, "SPIFFS unmounted");
}