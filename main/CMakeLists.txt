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
        INCLUDE_DIRS "."
        EMBED_TXTFILES ${project_dir}/certs/ca_cert.pem
        EMBED_TXTFILES ${project_dir}/certs/root_ca_cert.pem
        EMBED_TXTFILES ${project_dir}/certs/private.pem)