# The test list: one executable per area (tests/<name>.cpp), each with its layer, area and tier. A new test is added here and nowhere else.
sub0fabric_add_test(test_version pure)
sub0fabric_add_test(test_codec pure)
sub0fabric_add_test(test_segment pure)
sub0fabric_add_test(test_order pure)
sub0fabric_label_tests(AREA pure TIER smoke TARGETS test_version test_codec test_segment test_order)
