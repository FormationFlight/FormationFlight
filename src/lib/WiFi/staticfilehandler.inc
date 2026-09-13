
    server->on("/bundle.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", (uint8_t *)html_bundle_js_gz, sizeof(html_bundle_js_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/components.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", (uint8_t *)html_components_js_gz, sizeof(html_components_js_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/follow-logic.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", (uint8_t *)html_follow_logic_js_gz, sizeof(html_follow_logic_js_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/follow.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", (uint8_t *)html_follow_js_gz, sizeof(html_follow_js_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/formationflight.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/css", (uint8_t *)html_formationflight_css_gz, sizeof(html_formationflight_css_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/history.min.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", (uint8_t *)html_history_min_js_gz, sizeof(html_history_min_js_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/images/logo.svg", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "image/svg+xml", (uint8_t *)html_images_logo_svg_gz, sizeof(html_images_logo_svg_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", (uint8_t *)html_index_html_gz, sizeof(html_index_html_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", (uint8_t *)html_index_html_gz, sizeof(html_index_html_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/main.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", (uint8_t *)html_main_js_gz, sizeof(html_main_js_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server->on("/tailwind.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/css", (uint8_t *)html_tailwind_css_gz, sizeof(html_tailwind_css_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });
