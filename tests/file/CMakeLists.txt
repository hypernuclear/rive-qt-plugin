riveqt_add_headless_test(tst_rive_file SUITE File SLOTS
    importsValidFile
    enumeratesViewModels
    createsDefaultArtboard
    createsNamedArtboard
    missingArtboardReturnsNull
    malformedBytesFailWithError
    sameUrlIsCachedSameInstance)
