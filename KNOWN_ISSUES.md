# Known Issues

## UI lag with large sequences
- **Issue**: The user interface may become laggy or even freeze when loading or displaying large sequences.
- **Workaround**: Try to make the sequence smaller by reducing:
  - Number of slices, number of repetitions, number of diffusion directions, etc. 
  - Use the TR-segmented render mode instead of rendering the entire sequence, this will need the TR or RepetitionTime definition in the .seq file.

## K-space and M1 main-pathway approximation
- **Issue**: K-space trajectory and M1 calculations only account for the main pathway. Alternate coherence pathways or sequence-specific pathway selection are not modeled.
- **Workaround**: Treat K-space and M1 plots as main-pathway estimates, and validate pathway-dependent behavior with dedicated simulation or reconstruction tools.
