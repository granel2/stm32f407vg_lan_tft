# Prints FLASH/RAM usage as a small progress bar instead of raw byte counts.
# Invoked as a build-time script (cmake -P) from the "print_size" custom
# target in CMakeLists.txt, which passes:
#   SIZE_TOOL   - path to arm-none-eabi-size (or equivalent)
#   ELF_FILE    - path to the linked .elf
#   FLASH_SIZE  - FLASH region size in bytes (must match linker_script.ld)
#   RAM_SIZE    - RAM region size in bytes (must match linker_script.ld)
#
# Plain ASCII only (#/.) - block-drawing characters can come out mangled on
# a default-codepage Windows console, ASCII always renders correctly.

execute_process(
  COMMAND ${SIZE_TOOL} ${ELF_FILE}
  OUTPUT_VARIABLE size_output
  RESULT_VARIABLE size_result
)
if(NOT size_result EQUAL 0)
  message(FATAL_ERROR "print_size: '${SIZE_TOOL} ${ELF_FILE}' failed")
endif()

# Berkeley `size` output, 2nd line: "   text    data     bss     dec ..."
string(REGEX MATCH "([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)[ \t]+[0-9]+[ \t]+[0-9a-fA-F]+"
       _match "${size_output}")
if(NOT _match)
  message(FATAL_ERROR "print_size: could not parse size output:\n${size_output}")
endif()
set(text_bytes "${CMAKE_MATCH_1}")
set(data_bytes "${CMAKE_MATCH_2}")
set(bss_bytes  "${CMAKE_MATCH_3}")

math(EXPR flash_used "${text_bytes} + ${data_bytes}")
math(EXPR ram_used   "${data_bytes} + ${bss_bytes}")

# One-decimal-place percentage/KB via x10 integer math (math() is int-only).
function(format_x10 value out_var)
  math(EXPR whole "${value} / 10")
  math(EXPR frac  "${value} % 10")
  set(${out_var} "${whole}.${frac}" PARENT_SCOPE)
endfunction()

function(print_bar label used total)
  math(EXPR pct_x10  "(${used} * 1000) / ${total}")
  math(EXPR bar_x10  "(${used} * 300) / ${total}")   # 30-char bar, x10 for rounding
  math(EXPR filled   "(${bar_x10} + 5) / 10")          # round to nearest char
  if(filled GREATER 30)
    set(filled 30)
  endif()
  math(EXPR empty "30 - ${filled}")

  string(REPEAT "#" ${filled} filled_str)
  string(REPEAT "." ${empty} empty_str)

  format_x10(${pct_x10} pct_str)
  math(EXPR used_kb_x10  "(${used} * 10) / 1024")
  math(EXPR total_kb_x10 "(${total} * 10) / 1024")
  format_x10(${used_kb_x10} used_kb_str)
  format_x10(${total_kb_x10} total_kb_str)

  # Pad percentage to 5 chars ("100.0" is the widest) so the KB column lines up.
  string(LENGTH "${pct_str}" pct_len)
  math(EXPR pad_len "5 - ${pct_len}")
  if(pad_len GREATER 0)
    string(REPEAT " " ${pad_len} pct_pad)
  else()
    set(pct_pad "")
  endif()

  message("  ${label}  [${filled_str}${empty_str}]  ${pct_pad}${pct_str}%   ${used_kb_str} / ${total_kb_str} KB")
endfunction()

message("")
print_bar("FLASH" ${flash_used} ${FLASH_SIZE})
print_bar("RAM  " ${ram_used}   ${RAM_SIZE})
message("")
