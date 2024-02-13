#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <wchar.h>
#include <locale.h>

#include <curl/curl.h>
#include <jansson.h>


#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define ERASE_CURRENT_LINE "[2K\r"
#define ERASE_LINE_ABOVE "\33[2K\r"

#define HIDE_CURSOR "\33[?25l"
#define ENABLE_CURSOR "\33[?25h"

#define BUFFER_SIZE (512 * 1024)
#define PARAM_BUFFER_SIZE 512

#define GEOCODING_URL "http://api.openweathermap.org/geo/1.0/direct"
#define POLLUTION_URL "http://api.openweathermap.org/data/2.5/air_pollution"

#define QUERY_PARAM_STRING 1 << 1
#define QUERY_PARAM_DOUBLE 2 << 1

#define POLLUTANTS_COUNT 8

#define REQ_RETRY_ATTEMPTS 4U

#define RESP_OK 0U
#define RESP_ERR_UNAUTHENTICATED 1U
#define RESP_ERR_BAD_REQUEST 1 << 1U


typedef struct {
    size_t len;
    char* data;
} response_t;

typedef struct {
    uint32_t res_code;
    char* message;
} response_error_t;

typedef struct {
    double lat;
    double lon;
    char* city_name;
} geodata_t;

typedef struct {
    double value;
    // Report meta data [ Unused bits | severity (color bubble) 3 bits (0-4) | up / down 1 bit ]
    uint32_t meta; 
} pollutant_value_t;

typedef struct {
    double* components;
    uint8_t aqi;
} pollutants_t;

static char* POLLUTANT_REPORT_COMPONENTS[POLLUTANTS_COUNT] = {
    "co", "no", "no2", "o3", "so2", "nh3", "pm2_5", "pm10"
};

static char* POLLUTANT_LABELS_TABLE[POLLUTANTS_COUNT] = {
    "Carbone monoxide (CO)  ",
    "Nitrogen monoxide (NO) ", 
    "Nitrogen dioxide (NO₂) ", 
    "Ozone (O₃)             ", 
    "Sulphur dioxide (SO₂)  ", 
    "Ammonia (NH₃)          ", 
    "Particular matter 2.5µm", 
    "Particular matter 10µm "
};

static uint32_t RANGES_TABLE[POLLUTANTS_COUNT][4] = {
    { 4400, 9400, 12400, 15400 },  // [0] CO
    { 20,   40,   60,    80    },  // [1] NO
    { 40,   70,   150,   200   },  // [2] NO₂
    { 60,   100,  140,   180   },  // [3] O₃
    { 20,   80,   250,   350   },  // [4] SO₂
    { 40,   80,   120,   160   },  // [5] NH₃
    { 10,   25,   50,    75    },  // [6] PM 2.5
    { 20,   50,   100,   200   }   // [7] PM 10
};

static char* COLORS_TABLE[5] = {
    ANSI_COLOR_CYAN,
    ANSI_COLOR_GREEN,
    ANSI_COLOR_YELLOW,
    ANSI_COLOR_RED,
    ANSI_COLOR_MAGENTA,
};

static char* LABELS_TABLE[5] = {
    "Good",
    "Fair",
    "Moderate",
    "Poor",
    "Very Poor"
};


void
write_response(void *ptr, size_t size, size_t nmemb, void *stream) {
    size_t real_size = size * nmemb;
    response_t* response = (response_t*) stream;
    memcpy(response->data, ptr, real_size);
    response->data[real_size] = '\0';
    response->len = real_size;
}


static char*
make_query_param(char* key, void* value, uint8_t value_type) {
    size_t param_size;
    char* param_ptr = NULL;

    switch (value_type) {
        case QUERY_PARAM_STRING:
            // Lengths + "=" + "\0"
            param_size = strlen(key) + strlen((char*) value) + 2 * sizeof(char);
            param_ptr = malloc(param_size);
            snprintf(param_ptr, param_size, "%s=%s", key, (char*)value);
            break;
        case QUERY_PARAM_DOUBLE:
            // 4 bytes for signed int (0-180) + 2 bytes for the fractional part + "." + "=" + "\0"
            param_size = strlen(key) + (9 * sizeof(char));
            param_ptr = malloc(param_size);
            snprintf(param_ptr, param_size, "%s=%.2f", key, *((double*) value));
            break;
        default:
            fprintf(stderr, "%s Invalid query parameter type. %s\n", ANSI_COLOR_RED, ANSI_COLOR_RESET);
    }

    return param_ptr;
}


static uint8_t
make_request(CURL* curl, CURLU* url, response_t* response) {
    uint32_t res_code;

    curl_easy_setopt(curl, CURLOPT_CURLU, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res_code);
    curl_easy_reset(curl);

    return res_code == 200 ? 0 : 1;
}


static pollutants_t*
fetch_pollution_report(CURL* curl, geodata_t* geodata, char* api_key, response_error_t* response_error) {
    if (geodata == NULL) return NULL;

    json_error_t error;
    pollutants_t* report = NULL;
    json_t* report_root = NULL;

    response_t pollution_response = {
        .data = malloc(BUFFER_SIZE),
        .len = 0
    };

    CURLU* pollution_url = curl_url();
    curl_url_set(pollution_url, CURLUPART_URL, POLLUTION_URL, 0);
    curl_url_set(
        pollution_url, 
        CURLUPART_QUERY, 
        make_query_param("lat", &geodata->lat, QUERY_PARAM_DOUBLE),
        0
    );
    curl_url_set(
        pollution_url,
        CURLUPART_QUERY,
        make_query_param("lon", &geodata->lon, QUERY_PARAM_DOUBLE),
        CURLU_APPENDQUERY
    );

    curl_url_set(
        pollution_url,
        CURLUPART_QUERY, 
        make_query_param("appid", api_key, QUERY_PARAM_STRING),
        CURLU_APPENDQUERY
    );

    uint8_t status = make_request(curl, pollution_url, &pollution_response);
    if (status != 0) {
        json_t* error_root = json_loads(pollution_response.data, 0, &error);
        json_t* message = json_object_get(error_root, "message");
        response_error->message = message ? (char*) json_string_value(message) : "Failed to fetch pollution report";
        goto fail;
    }

    report_root = json_loads(pollution_response.data, 0, &error);
    if (report_root == NULL) {
        response_error->message = "Failed to parse response";
        goto fail;
    }

    json_t* list = json_object_get(report_root, "list");
    json_t* obj  = json_array_get(list, 0);
    json_t* main = json_object_get(obj, "main");
    json_t* aqi  = json_object_get(main, "aqi");

    json_t* components = json_object_get(obj, "components");

    uint8_t aqi_value = json_integer_value(aqi);
    if (aqi_value < 1 || aqi_value > 5) {
        response_error->message = "Invalid aqi_value";
        goto fail;
    }

    report = malloc(sizeof(pollutants_t));
    report->components = malloc(sizeof(double) * POLLUTANTS_COUNT);
    report->aqi        = aqi_value;

    double* report_ptr = report->components;
    for (uint8_t i = 0; i < POLLUTANTS_COUNT; i++, report_ptr++) {
        *report_ptr = json_real_value(
            json_object_get(components, POLLUTANT_REPORT_COMPONENTS[i])
        );
    }

    return report;

    fail:
        if (report) free(report);
        if (report_root) json_decref(report_root);
        return NULL;
}


static geodata_t*
fetch_geodata(CURL* curl, char* city_name, char* api_key, response_error_t* response_error) {
    json_error_t error;
    geodata_t* geodata = NULL;
    json_t* response_root = NULL;

    response_t geocoding_response = {
        .data = malloc(BUFFER_SIZE),
        .len = 0
    };

    CURLU* geocoding_url = curl_url();
    curl_url_set(geocoding_url, CURLUPART_URL, GEOCODING_URL, 0);
    curl_url_set(geocoding_url, CURLUPART_QUERY, make_query_param("q", city_name, QUERY_PARAM_STRING), 0);
    curl_url_set(geocoding_url, CURLUPART_QUERY, "limit=1", CURLU_APPENDQUERY);
    curl_url_set(
        geocoding_url,
        CURLUPART_QUERY,
        make_query_param("appid", api_key, QUERY_PARAM_STRING),
        CURLU_APPENDQUERY
    );

    json_t* target = NULL;
    for (uint8_t i = 0; i <= REQ_RETRY_ATTEMPTS; i++) {
        if (i) sleep(1 << (i - 1));

        uint8_t status = make_request(curl, geocoding_url, &geocoding_response);
        if (status != 0) continue;

        response_root = json_loads(geocoding_response.data, 0, &error);
        if (response_root == NULL) continue;

        target = json_array_get(response_root, 0);
        if (target == NULL) continue;

        break;
    }

    if (target == NULL) {
        if (response_root == NULL) goto fail;
        json_t* message = json_object_get(response_root, "message");
        response_error->message = message ? (char*) json_string_value(message) : "Failed to fetch geodata";
        goto fail;
    }

    geodata = malloc(sizeof(geodata_t));

    const char* name = json_string_value(json_object_get(target, "name"));
    size_t name_size = sizeof(char) * strlen(name);
    geodata->city_name = malloc(name_size);

    memcpy((void*)geodata->city_name, name, name_size);

    geodata->lat = json_real_value(json_object_get(target, "lat"));
    geodata->lon = json_real_value(json_object_get(target, "lon"));

    if (geodata->lat == 0 || geodata->lon == 0) goto fail;

    json_decref(response_root);

    return geodata;

    fail:
        if (geodata) free(geodata);
        if (response_root) json_decref(response_root);
        return NULL;
}


static inline void
print_color_tag(char* color) {
    printf("%s", color);
    wprintf(L"%lc", 0x25cf);
    printf(ANSI_COLOR_RESET);
}


static void
print_pollutant_row(char* label, double value, uint8_t ranges_table_index) {
    uint8_t i = 0;
    uint32_t cmp_value = (uint32_t) value;
    while (i < 4 && RANGES_TABLE[ranges_table_index][i] < cmp_value) i++;

    printf("\t");
    printf("%s \t", label);
    print_color_tag(COLORS_TABLE[i]);
    printf(" %.2f µg/m3 \n", value);

}


static inline void
print_legend() {
    printf("\n\n\t");
    for (uint8_t i = 0; i < 5; i++) {
        print_color_tag(COLORS_TABLE[i]);
        printf(" %s  ", LABELS_TABLE[i]);   
    }
    printf("\n\n");
}


static inline void
print_loader_fragment(size_t code, const char* label, struct timespec* ts) {
    wprintf(L"%s[%lc]%s", ANSI_COLOR_MAGENTA, code, ANSI_COLOR_RESET);
    fflush(stdout);
    nanosleep(ts, NULL);
    printf("\r%s ", label);
    fflush(stdout);
}


static void*
show_loader(void* is_active) {
    flockfile(stdout);
    uint8_t* active = (uint8_t*)is_active;
    const char* label = "Loading air quality report..";
    uint32_t ms = 150;

    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    
    printf("\n\r%s ", label);
    fflush(stdout);
    
    while (1) {
        if (! *active) break;
        print_loader_fragment(0x25E2, label, &ts);
        print_loader_fragment(0x25E3, label, &ts);
        print_loader_fragment(0x25E4, label, &ts);
        print_loader_fragment(0x25E5, label, &ts);
    }

    printf(ERASE_LINE_ABOVE);
    fflush(stdout);

    funlockfile(stdout);
    pthread_exit(NULL);
    return NULL;
}


static inline void
print_report(char* city_name, pollutants_t* report) {
    printf("\tAir quality in %s ", city_name);
    print_color_tag(COLORS_TABLE[report->aqi - 1]);
    printf("\n\n\tAQI %hhu (%s) \n\n", report->aqi, LABELS_TABLE[report->aqi - 1]);

    double* report_ptr = report->components;
    for (uint8_t i = 0; i < POLLUTANTS_COUNT; i++, report_ptr++) {
        print_pollutant_row(POLLUTANT_LABELS_TABLE[i], *report_ptr, i);
    }

    print_legend();
}

int
main(int argc, char const **argv, char const **env) {
     setlocale(LC_CTYPE, "");

    /* -- Initialisation -- */
    char* failure_reason = NULL;
    uint8_t loader_active = 0;

    CURL* curl = NULL;
    pthread_t* loader_thread_id = NULL;

    response_error_t response_error = {
        .res_code = 0,
        .message = NULL
    };

    /* -- / initialisation -- */

    if (getenv("API_KEY") == NULL) {
        failure_reason = "Please provide API_KEY";
        goto exit;
    }

    char* api_key = strndup(getenv("API_KEY"), PARAM_BUFFER_SIZE);

    curl = curl_easy_init();
    curl_global_init(CURL_GLOBAL_ALL);

    if (curl == NULL) {
        failure_reason = "Failed to initialize curl.";
        goto exit;
    }

    if (argc < 2) {
        failure_reason = "No city name provided.";
        goto exit;
    }

    char* city_name = strndup(argv[1], PARAM_BUFFER_SIZE);

    /* -- Starting loader -- */
    /* -- Stdout is locked -- */
    printf(HIDE_CURSOR);
    loader_active = 1;
    loader_thread_id = malloc(sizeof(pthread_t));
    pthread_create(loader_thread_id, NULL, &show_loader, &loader_active);

    /* -- Fetching geodata (correct city name, lat and lon) -- */
    geodata_t* geodata = fetch_geodata(curl, city_name, api_key, &response_error);
    if (geodata == NULL) {
        failure_reason = response_error.message;
        goto exit;
    }

    /* -- Fetching pollution report -- */
    pollutants_t* report = fetch_pollution_report(curl, geodata, api_key, &response_error);
    if (report == NULL) {
        failure_reason = response_error.message;
        goto exit;
    }

    /* -- Removing loader -- */
    /* -- Stdout is unlocked -- */
    loader_active = 0;
    printf(ENABLE_CURSOR);

    print_report(geodata->city_name, report);
    free(geodata);
    free(report);

    exit:
        if (loader_active) loader_active = 0;
        printf(ERASE_LINE_ABOVE);
        printf(ENABLE_CURSOR);

        curl_easy_cleanup(curl);
        curl_global_cleanup();

        if (failure_reason) {
            fprintf(stderr, "%s%s%s\n\n", ANSI_COLOR_RED, failure_reason, ANSI_COLOR_RESET);
            fflush(stderr);
            exit(1);
        }

        exit(0);
}
