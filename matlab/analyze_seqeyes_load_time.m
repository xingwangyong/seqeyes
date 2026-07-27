function summary = analyze_seqeyes_load_time(csvFile, outFigure)
%ANALYZE_SEQEYES_LOAD_TIME Plot SeqEyes load/render time versus .seq file size.
%
% summary = analyze_seqeyes_load_time(csvFile)
% summary = analyze_seqeyes_load_time(csvFile, outFigure)
%
% csvFile is produced by tools/measure_seqeyes_load_time.sh.
% If multiple rows exist for the same seq_file, total_ms is averaged.

if nargin < 1 || isempty(csvFile)
    csvFile = "seqeyes_load_time.csv";
end
if nargin < 2
    outFigure = "";
end

T = readtable(csvFile, "TextType", "string");
requiredVars = ["seq_file", "seq_name", "total_ms"];
assert(all(ismember(requiredVars, string(T.Properties.VariableNames))), ...
    "Input CSV must contain columns: %s", strjoin(requiredVars, ", "));

T.file_size_mb = arrayfun(@localFileSizeMb, T.seq_file);

validRows = ~isnan(T.total_ms) & ~isnan(T.file_size_mb);
if ~all(validRows)
    warning("Ignoring %d rows with missing total_ms or file size.", nnz(~validRows));
end
T = T(validRows, :);

[groups, seqFiles] = findgroups(T.seq_file);
[~, baseNames, extensions] = arrayfun(@fileparts, seqFiles, "UniformOutput", false);

summary = table();
summary.seq_file = seqFiles;
summary.seq_name = string(baseNames) + string(extensions);
summary.file_size_mb = splitapply(@localMeanOmitNan, T.file_size_mb, groups);
summary.mean_total_ms = splitapply(@localMeanOmitNan, T.total_ms, groups);
summary.mean_total_s = summary.mean_total_ms / 1000;
summary.std_total_ms = splitapply(@localStdOmitNan, T.total_ms, groups);
summary.std_total_s = summary.std_total_ms / 1000;
summary.n_runs = splitapply(@numel, T.total_ms, groups);

summary = sortrows(summary, "file_size_mb");

figure("Name", "SeqEyes Load Time vs File Size", "Color", "w");
hold on;
box on;

h = plot(summary.file_size_mb, summary.mean_total_s, "o", ...
    "LineStyle", "none", ...
    "MarkerSize", 7, ...
    "LineWidth", 0.9);
h.MarkerFaceColor = h.Color;
h.MarkerEdgeColor = h.Color;

xlabel(".seq file size (MB)");
ylabel("Load + render time (s)");
title("Load+render time");

ax = gca;
ax.FontName = "Helvetica";
ax.FontSize = 12;
ax.LineWidth = 1;
ax.TickDir = "out";
ax.Box = "off";
ax.Color = [0.985 0.985 0.975];
ax.XGrid = "on";
ax.YGrid = "on";
ax.GridColor = [0.82 0.82 0.78];
ax.GridAlpha = 0.35;

if strlength(string(outFigure)) > 0
    exportgraphics(gcf, outFigure, "Resolution", 200);
end
end

function sizeMb = localFileSizeMb(pathValue)
info = dir(pathValue);
if isempty(info)
    warning("File not found when computing size: %s", pathValue);
    sizeMb = NaN;
else
    sizeMb = info.bytes / 1024^2;
end
end

function y = localMeanOmitNan(x)
x = x(~isnan(x));
if isempty(x)
    y = NaN;
else
    y = mean(x);
end
end

function y = localStdOmitNan(x)
x = x(~isnan(x));
if numel(x) < 2
    y = NaN;
else
    y = std(x);
end
end
