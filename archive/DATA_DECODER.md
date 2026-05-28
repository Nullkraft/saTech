You gave me the range of the normalized data_samples from channel 2 for this capture = [0, 53]
So, ch2_max is measured per capture; for this run ch2_max = 53

Unless you have a better idea we are going to use the following steps to decode the 32 clock-edge sample points into a 32-bit binary word:

1. initialize data_word = 0
2. Verify num(sample_indexes) is exactly divisible by 32
    - If not then reread the already captured data and verify sample_indexes again
    - If it fails a second time ask me to trigger another scope sweep
3. Get each index from the list of sample_indexes (32 clock-edge sample points)
4. Bits are consumed MSB-first: each sample becomes the next bit, and the word is updated with data_word = (data_word << 1) | bit.
    If data_value[index] > (0.8 * ch2_max)
        - bit = 0x1
    If data_value[index] < (0.2 * ch2_max)
        - bit = 0x0
    Anything else is an error and requires triggering a new sweep.
5. When a 32-bit word is completed
    - Store it separately from any other 32-bit words that may be decoded.
    - Use the low 3 bits as the address and store it in the 8-entry map keyed by that address.
    - reinitialize data_word = 0
6. If a 32-bit word is repeated in the scope data just overwrite it.
