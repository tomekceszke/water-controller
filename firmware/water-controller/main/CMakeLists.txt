idf_build_get_property(project_dir PROJECT_DIR)
idf_component_register(
        SRCS "main.c"
        "base64url.c"
        "tools.c"
        "jwt.c"
        "wifi.c"
        "web.c"
        "ota.c"
        "gcp.c"
        "pcnt.c"
        "hw.c"
        INCLUDE_DIRS "."
        EMBED_TXTFILES ${project_dir}/certs/ota_server_cert_15.pem
        EMBED_TXTFILES ${project_dir}/certs/gcp_ca_cert.pem
        EMBED_TXTFILES ${project_dir}/certs/gcp_private_key.pem
        EMBED_FILES ${project_dir}/www/index.html.gz
)