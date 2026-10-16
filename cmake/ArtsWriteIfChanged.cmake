# A generated file's modification time is what the build compares against its
# dependents, so a configure that rewrites identical bytes recompiles every
# object that includes it.  These helpers touch the destination only when its
# content differs.

# arts_publish_if_changed(<staged> <dest>)
#
# Copies <staged> over <dest> unless the two are already byte-identical.
function(arts_publish_if_changed staged dest)
    configure_file("${staged}" "${dest}" COPYONLY)
endfunction()

# arts_write_if_changed(<dest> <content>)
#
# Writes <content> verbatim to <dest>, leaving <dest> untouched when it
# already holds exactly that.  The content is staged beside the destination
# as <dest>.tmp, so no substitution is applied to it.
function(arts_write_if_changed dest content)
    file(WRITE "${dest}.tmp" "${content}")
    arts_publish_if_changed("${dest}.tmp" "${dest}")
endfunction()
