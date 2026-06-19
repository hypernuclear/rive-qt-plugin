riveqt_add_headless_test(tst_rive_view_model SUITE ViewModel SLOTS
    definitionEnumeratesProperties
    instanceEnumeratesProperties
    typedAccessorsResolveByType
    typeMismatchReturnsNull
    accessorsAreCached
    colorRoundTrips
    stringWriteEmitsMutationAndPersists
    triggerFiresAndPollEmits
    numberRoundTrips)
