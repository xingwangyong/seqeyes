% Export MATLAB calcPNS output to CSV for direct comparison with SeqEyes.
% Required env vars:
%   PULSEQ_MATLAB_DIR : pulseq matlab folder containing +mr
%   SEQ_PATH          : .seq file path
%   ASC_PATH          : .asc hardware file path
%   OUT_CSV           : output CSV path
% Optional env vars:
%   SAMPLE_STRIDE     : keep every N-th sample

pulseqDir = getenv('PULSEQ_MATLAB_DIR');
seqPath = getenv('SEQ_PATH');
ascPath = getenv('ASC_PATH');
outCsv = getenv('OUT_CSV');
strideStr = getenv('SAMPLE_STRIDE');

if isempty(pulseqDir) || isempty(seqPath) || isempty(ascPath) || isempty(outCsv)
    error('Missing required environment variables for MATLAB PNS export.');
end

addpath(pulseqDir);

seq = mr.Sequence();
seq.read(seqPath);
[ok, pns_norm, pns_comp, t_axis] = seq.calcPNS(ascPath, false);

if size(pns_comp, 1) ~= 3
    error('Unexpected pns_comp shape from calcPNS.');
end

stride = str2double(strideStr);
if ~isfinite(stride) || stride < 1
    stride = 1;
end
stride = floor(stride);
idx = 1:stride:length(t_axis);
if idx(end) ~= length(t_axis)
    idx = [idx, length(t_axis)];
end

M = [t_axis(idx)', pns_comp(1,idx)', pns_comp(2,idx)', pns_comp(3,idx)', pns_norm(idx)'];
writematrix(M, outCsv);

fprintf('MATLAB_PNS_OK samples=%d max_norm=%f ok=%d\n', size(M,1), max(pns_norm), ok(1));

