#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

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

#define BUFFER_SIZE (512 * 1024)
#define GEOCODING_URL "http://api.openweathermap.org/geo/1.0/direct"
#define POLLUTION_URL "http://api.openweathermap.org/data/2.5/air_pollution"

typedef struct {
    char* data;
    size_t len;
} response_t;


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


static response_t*
make_request(CURL* curl, CURLU* url, char* api_key) {
    CURLcode curl_code;
    uint32_t res_code;

    response_t* response = malloc(sizeof(response_t));
    response->data = malloc(BUFFER_SIZE);
    response->len = 0;

    char* api_key_param = malloc(strlen(api_key) + 8);
    strcpy(api_key_param, "appid=");
    strcat(api_key_param, api_key);
    curl_url_set(url, CURLUPART_QUERY, api_key_param, CURLU_APPENDQUERY);

    curl_easy_setopt(curl, CURLOPT_CURLU, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

    free(api_key_param);

    curl_code = curl_easy_perform(curl);
/*    if (curl_code != CURLE_OK) {
        fprintf(stderr, "%s \n", curl_easy_strerror(curl_code));
        goto fail;
    };*/

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res_code);
    curl_easy_reset(curl);

    if (res_code != 200) goto fail;
    return response;

    fail:
        free(response);
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
    printf("\n\n");
    for (uint8_t i = 0; i < 5; i++) {
        print_color_tag(COLORS_TABLE[i]);
        printf(" %s  ", LABELS_TABLE[i]);   
    }
    printf("\n\n");
}

int
main(int argc, char const **argv, char const **env) {
    setlocale(LC_CTYPE, "");
    char* failure_reason = NULL;
    json_error_t error;

    char* city_name_param = NULL;

    char* lat_param = NULL;
    char* lon_param = NULL;

    CURLU* geocoding_url = curl_url();
    CURLU* pollution_url = curl_url();

    response_t* geocoding_response = NULL;
    response_t* pollution_response = NULL;

    json_t* coords_root = NULL;
    json_t* report_root = NULL;

    CURL* curl = curl_easy_init();

    if (curl == NULL) {
        failure_reason = "Failed to initialize curl.";
        goto exit;
    }

    if (argc < 2) {
        failure_reason = "No city name provided.";
        goto exit;
    }

    char* api_key = getenv("API_KEY"); 
    const char* city_name = argv[1];

    uint16_t city_name_param_size = strlen(city_name) + 3;
    city_name_param = malloc(city_name_param_size);
    snprintf(city_name_param, city_name_param_size, "q=%s", city_name);

    curl_url_set(geocoding_url, CURLUPART_URL, GEOCODING_URL, 0);
    curl_url_set(geocoding_url, CURLUPART_QUERY, city_name_param, 0);
    curl_url_set(geocoding_url, CURLUPART_QUERY, "limit=1", CURLU_APPENDQUERY);

    geocoding_response = make_request(curl, geocoding_url, api_key);
    if (geocoding_response == NULL) goto no_geo_data;

    coords_root = json_loads(geocoding_response->data, 0, &error);
    if (coords_root == NULL) goto no_geo_data;

    json_t* target = json_array_get(coords_root, 0);
    if (target == NULL) goto no_geo_data;

    const char* returned_city_name = json_string_value(json_object_get(target, "name"));

    double lat = json_real_value(json_object_get(target, "lat"));
    double lon = json_real_value(json_object_get(target, "lon"));
    if (lat == 0 || lon == 0) goto no_geo_data;

    uint16_t param_size = sizeof(char) * 12 + 1;
    lat_param = malloc(param_size);
    lon_param = malloc(param_size);

    snprintf(lat_param, param_size, "lat=%.2f", lat);
    snprintf(lon_param, param_size, "lon=%.2f", lon);

    curl_url_set(pollution_url, CURLUPART_URL, POLLUTION_URL, 0);
    curl_url_set(pollution_url, CURLUPART_QUERY, lat_param, 0);
    curl_url_set(pollution_url, CURLUPART_QUERY, lon_param, CURLU_APPENDQUERY);

    pollution_response = make_request(curl, pollution_url, api_key);
    if (pollution_response == NULL) goto no_pollution_data;

    report_root = json_loads(pollution_response->data, 0, &error);
    if (report_root == NULL) goto no_pollution_data;

    // Use json_unpack
    json_t* list = json_object_get(report_root, "list");
    json_t* obj = json_array_get(list, 0);
    json_t* main = json_object_get(obj, "main");
    json_t* aqi = json_object_get(main, "aqi");

    json_t* components = json_object_get(obj, "components");

    uint8_t aqi_value = json_integer_value(aqi);
    if (aqi_value < 1 || aqi_value > 5) {
        failure_reason = "Incorrect AQI index";
        goto exit;
    }

    printf("\nAir quality in %s ", returned_city_name);
    print_color_tag(COLORS_TABLE[aqi_value - 1]);
    printf("\n\nAQI %hhu (%s) \n\n", aqi_value, LABELS_TABLE[aqi_value - 1]);

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

    no_geo_data:
        failure_reason = "Cannot fetch geodata, please try again.";
        goto exit;

    no_pollution_data:
        failure_reason = "Cannot fetch pollution data, please try again.";
        goto exit;

    exit:
        fflush(stdout);

        curl_easy_cleanup(curl);
        curl_global_cleanup();

        if (failure_reason) {
            fprintf(stderr, "\n%s%s%s\n\n", ANSI_COLOR_RED, failure_reason, ANSI_COLOR_RESET);
            exit(1);
        }

        return 0;
}
