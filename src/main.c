#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include <unistd.h>
#include <pthread.h>

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


typedef struct {
    char* data;
    size_t len;
} response_t;

typedef struct {
    double lat;
    double lon;
    const char* city_name;
} geodata_t;

typedef struct {
    double co;
    double no;
    double no3;
    double o3;
    double so2;
    double nh3;
    double pm2_5;
    double pm10;
} pollutants_t;


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

static uint32_t RANGES_TABLE[8][4] = {
    { 4400, 9400, 12400, 15400 },  // [0] CO
    { 20,   40,   60,    80    },  // [1] NO
    { 40,   70,   150,   200   },  // [2] NO₂
    { 60,   100,  140,   180   },  // [3] O₃
    { 20,   80,   250,   350   },  // [4] SO₂
    { 40,   80,   120,   160   },  // [5] NH₃
    { 10,   25,   50,    75    },  // [6] PM 2.5
    { 20,   50,   100,   200   }   // [7] PM 10
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

static response_t*
make_request(CURL* curl, CURLU* url) {
    uint32_t res_code;

    response_t* response = malloc(sizeof(response_t));
    response->data = malloc(BUFFER_SIZE);
    response->len = 0;

    curl_easy_setopt(curl, CURLOPT_CURLU, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res_code);
    curl_easy_reset(curl);

    if (res_code != 200) {
        free(response);
        fprintf(
            stderr,
            "The remote host responded with the error status code %s (%d) %s \n",
            ANSI_COLOR_YELLOW, res_code, ANSI_COLOR_RESET
        );
        return NULL;
    }

    return response;
}


static geodata_t*
fetch_geodata(CURL* curl, char* city_name, char* api_key) {
    uint8_t retry_attempts = 3;
    json_error_t error;
    geodata_t* geodata = NULL;
    json_t* coords_root = NULL;
    response_t* geocoding_response  = NULL;

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
    for (uint8_t i = retry_attempts; i > 0; i--) {
        geocoding_response = make_request(curl, geocoding_url);
        if (geocoding_response == NULL) goto fail;

        coords_root = json_loads(geocoding_response->data, 0, &error);
        if (coords_root == NULL) goto fail;

        target = json_array_get(coords_root, 0);
        if (target != NULL) break;

        sleep(1);
    }

    if (target == NULL) goto fail;

    geodata = malloc(sizeof(geodata_t));

    const char* name = json_string_value(json_object_get(target, "name"));
    size_t name_size = sizeof(char) * strlen(name);
    geodata->city_name = malloc(name_size);

    memcpy((void*)geodata->city_name, name, name_size);

    geodata->lat = json_real_value(json_object_get(target, "lat"));
    geodata->lon = json_real_value(json_object_get(target, "lon"));

    // Implement retry
    if (geodata->lat == 0 || geodata->lon == 0) goto fail;

    free(geocoding_response);
    json_decref(coords_root);

    return geodata;

    fail:
        if (geodata) free(geodata);
        if (geocoding_response) free(geocoding_response);
        if (coords_root) json_decref(coords_root);
        return NULL;
}


static inline void
print_color_tag(char* color) {
    printf("%s", color);
    wprintf(L"%lc", 0x25cf);
    printf(ANSI_COLOR_RESET);
}

static void
print_pollutant_row(char* label, json_t* pollutant_volume, uint8_t ranges_table_index) {
    double value = json_real_value(pollutant_volume);

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
print_loader_fragment(size_t code, char* label, struct timespec* ts) {
    wprintf(L"%s[%lc]%s", ANSI_COLOR_MAGENTA, code, ANSI_COLOR_RESET);
    fflush(stdout);
    nanosleep(ts, NULL);
    printf("\r%s ", label);
    fflush(stdout);
}


static void*
show_loader(void* label) {
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    uint32_t ms = 150;

    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    
    printf(HIDE_CURSOR);
    printf("\n\r%s ", (char*)label);
    fflush(stdout);
    
    while (1) {
        print_loader_fragment(0x25E2, (char*)label, &ts);
        print_loader_fragment(0x25E3, (char*)label, &ts);
        print_loader_fragment(0x25E4, (char*)label, &ts);
        print_loader_fragment(0x25E5, (char*)label, &ts);
    }

    printf(ENABLE_CURSOR);
    fflush(stdout);
}

int
main(int argc, char const **argv, char const **env) {
    /* -- Initialisation -- */
    char* failure_reason = NULL;
    json_error_t error;

    CURLU* pollution_url = curl_url();
    response_t* pollution_response = NULL;
    json_t* report_root = NULL;

    pthread_t* loader_thread_id = NULL;

    setlocale(LC_CTYPE, "");

    char* api_key_env = getenv("API_KEY");
    size_t api_key_len = (strnlen(api_key_env, PARAM_BUFFER_SIZE) + 1) * sizeof(char);
    char* api_key = malloc(api_key_len);
    strncpy(api_key, api_key_env, api_key_len);

    CURL* curl = curl_easy_init();
    curl_global_init(CURL_GLOBAL_ALL);

    if (curl == NULL) {
        failure_reason = "Failed to initialize curl.";
        goto exit;
    }

    if (api_key == NULL) {
        failure_reason = "Please provide API_KEY";
        goto exit;
    }

    if (argc < 2) {
        failure_reason = "No city name provided.";
        goto exit;
    }

    // TODO: Abstract param bufferisation
    uint32_t param_size = strnlen(argv[1], PARAM_BUFFER_SIZE);
    char* city_name = malloc(param_size);
    strncpy(city_name, argv[1], param_size);

    /* -- Starting loader -- */
    loader_thread_id = malloc(sizeof(pthread_t));
    pthread_create(loader_thread_id, NULL, &show_loader, "Loading data");

    /* -- Fetching geodata (correct city name, lat and lon) -- */
    geodata_t* geodata = fetch_geodata(curl, city_name, api_key);
    if (geodata == NULL) {
        failure_reason = "Cannot fetch geodata, please try again.";
        goto exit;
    }

    /* -- Fetching pollution data -- */
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

    pollution_response = make_request(curl, pollution_url);
    if (pollution_response == NULL) goto no_pollution_data;

    report_root = json_loads(pollution_response->data, 0, &error);
    if (report_root == NULL) goto no_pollution_data;

    json_t* list = json_object_get(report_root, "list");
    json_t* obj  = json_array_get(list, 0);
    json_t* main = json_object_get(obj, "main");
    json_t* aqi  = json_object_get(main, "aqi");

    json_t* components = json_object_get(obj, "components");

    uint8_t aqi_value = json_integer_value(aqi);
    if (aqi_value < 1 || aqi_value > 5) {
        failure_reason = "Incorrect AQI index";
        goto exit;
    }

    /* -- Removing loader -- */
    pthread_cancel(*loader_thread_id);
    printf(ERASE_LINE_ABOVE);
    printf(ENABLE_CURSOR);
    fflush(stdout);

    /* -- Printing the report -- */
    printf("Air quality in %s ", geodata->city_name);
    print_color_tag(COLORS_TABLE[aqi_value - 1]);
    printf("\n\n\tAQI %hhu (%s) \n\n", aqi_value, LABELS_TABLE[aqi_value - 1]);

    print_pollutant_row("Carbone monoxide (CO)  ",  json_object_get(components, "co"),     0);
    print_pollutant_row("Nitrogen monoxide (NO) ",  json_object_get(components, "no"),     1);
    print_pollutant_row("Nitrogen dioxide (NO₂) ",  json_object_get(components, "no2"),    2);
    print_pollutant_row("Ozone (O₃)             ",  json_object_get(components, "o3"),     3);
    print_pollutant_row("Sulphur dioxide (SO₂)  ",  json_object_get(components, "so2"),    4);
    print_pollutant_row("Ammonia (NH₃)          ",  json_object_get(components, "nh3"),    5);
    print_pollutant_row("Particular matter 2.5µm",  json_object_get(components, "pm2_5"),  6);
    print_pollutant_row("Particular matter 10µm ",  json_object_get(components, "pm10"),   7);

    print_legend();

    goto exit;

    no_pollution_data:
        failure_reason = "Cannot fetch pollution data, please try again.";
        goto exit;

    exit:
        free(api_key);
        // TODO: Handle signals gracefully
        printf(ENABLE_CURSOR);
        fflush(stdout);

        if (loader_thread_id != NULL) {
            pthread_cancel(*loader_thread_id);
            printf(ERASE_LINE_ABOVE);
            fflush(stdout);
        }

        curl_easy_cleanup(curl);
        curl_global_cleanup();

        if (failure_reason) {
            fprintf(stderr, "%s%s%s\n\n", ANSI_COLOR_RED, failure_reason, ANSI_COLOR_RESET);
            exit(1);
        }

        exit(0);
}
