/* print a primitive into FD, fails if term is not primitive */
int b_PICAT_PRINT_PRIMITIVE_cc(BPLONG FDIndex, BPLONG term) {
    int res;
    BPLONG old_out_file_i;

    old_out_file_i = out_file_i;
    DEREF(FDIndex);
    out_file_i = INTVAL(FDIndex);
    CHECK_FILE_INDEX(out_file_i);
    curr_out = file_table[out_file_i].fdes;

    res = b_WRITE_QUICK_c(term);

    out_file_i = old_out_file_i;
    return res;
}

/* write a primitive into FD, fails if term is not primitive */
int b_PICAT_WRITE_PRIMITIVE_cc(BPLONG FDIndex, BPLONG term) {
    int res;
    BPLONG old_out_file_i;

    old_out_file_i = out_file_i;
    DEREF(FDIndex);
    out_file_i = INTVAL(FDIndex);
    CHECK_FILE_INDEX(out_file_i);
    curr_out = file_table[out_file_i].fdes;

    res = b_WRITEQ_QUICK_c(term);

    out_file_i = old_out_file_i;
    return res;
}


  
  
