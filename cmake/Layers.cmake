# Fabric's layers, lowest first (docs/DESIGN.md, "Layers"). Each is one static library, sub0fabric_<layer>, made from src/<layer>/*.cpp with headers
# under include/sub0fabric/<layer>/, and links only the layers below it.
#   pure  record framing and codecs, order, cursor, segment byte format: no I/O, no clock, no threads, no environment
#   io    the directory store over Sub0Platform: writer, reader, merge reader, the journal-1 facade
set(SUB0FABRIC_LAYERS pure io)
