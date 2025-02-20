IF (OPENSOURCE)
    IF (MODULE_KIND == PROTO_LIBRARY)
        EXCLUDE_TAGS(PY_PROTO)
    ENDIF()

    IF (MODULE_KIND == PY23_LIBRARY)
        EXCLUDE_TAGS(PY2)
    ENDIF()

    IF (MODULE_KIND == PY23_TEST)
        EXCLUDE_TAGS(PY2)
    ENDIF()

    LICENSE_RESTRICTION_EXCEPTIONS(
        # Java tests
        contrib/java/junit/junit/4.13
        contrib/java/javax/persistence/persistence-api/1.0
    )
ENDIF()
