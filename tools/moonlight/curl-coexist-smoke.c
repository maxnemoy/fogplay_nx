#include <curl/curl.h>

/* Link proof for the application's existing libnx curl and cookie support.
 * This function is only reached with the smoke executable's test argument. */
int moonlight_check_app_curl(void) {
    CURL *handle = curl_easy_init();
    if (!handle)
        return 1;
    CURLcode result = curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");
    struct curl_slist *cookies = NULL;
    if (result == CURLE_OK)
        result = curl_easy_getinfo(handle, CURLINFO_COOKIELIST, &cookies);
    curl_slist_free_all(cookies);
    curl_easy_cleanup(handle);
    return (int)result;
}
