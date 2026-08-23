idf_component_register(
    SRCS "fhss_ota_diagnostics.c"
    INCLUDE_DIRS "include"
    REQUIRES log ota_protocol
)
