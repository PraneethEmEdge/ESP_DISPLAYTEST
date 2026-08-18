idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES
                    esp_driver_gpio
                    esp_jpeg
                    EMBED_FILES
                        "frame001.jpg"
                        "frame002.jpg"
                        "frame003.jpg"
                        "frame004.jpg"
                        "frame005.jpg"
                    )
