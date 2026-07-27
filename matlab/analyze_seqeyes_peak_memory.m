function summary = analyze_seqeyes_peak_memory(csvFile, outFigure)
%ANALYZE_SEQEYES_PEAK_MEMORY Plot SeqEyes peak RSS versus .seq file size.
%
% summary = analyze_seqeyes_peak_memory(csvFile)
% summary = analyze_seqeyes_peak_memory(csvFile, outFigure)
%
% csvFile is produced by tools/measure_seqeyes_peak_memory.sh.
% If multiple runs exist for the same seq_file, max_rss_mb is averaged.

if nargin < 1 || isempty(csvFile)
    csvFile = "seqeyes_peak_memory.csv";
end
if nargin < 2
    outFigure = "";
end

T = readtable(csvFile, "TextType", "string");
requiredVars = ["seq_file", "seq_name", "max_rss_mb"];
assert(all(ismember(requiredVars, string(T.Properties.VariableNames))), ...
    "Input CSV must contain columns: %s", strjoin(requiredVars, ", "));

T.file_size_mb = arrayfun(@localFileSizeMb, T.seq_file);

[groups, seqFiles] = findgroups(T.seq_file);
[~, baseNames, extensions] = arrayfun(@fileparts, seqFiles, "UniformOutput", false);

summary = table();
summary.seq_file = seqFiles;
summary.seq_name = string(baseNames) + string(extensions);
summary.file_size_mb = splitapply(@localMeanOmitNan, T.file_size_mb, groups);
summary.mean_max_rss_mb = splitapply(@localMeanOmitNan, T.max_rss_mb, groups);
summary.mean_max_rss_gb = summary.mean_max_rss_mb / 1024;
summary.std_max_rss_mb = splitapply(@localStdOmitNan, T.max_rss_mb, groups);
summary.std_max_rss_gb = summary.std_max_rss_mb / 1024;
summary.n_runs = splitapply(@numel, T.max_rss_mb, groups);

summary = sortrows(summary, "file_size_mb");

figure("Name", "SeqEyes Peak RSS vs File Size", "Color", "w");
hold on;
box on;

h = plot(summary.file_size_mb, summary.mean_max_rss_gb, "o", ...
    "LineStyle", "none", ...
    "MarkerSize", 7, ...
    "LineWidth", 0.9);
h.MarkerFaceColor = h.Color;
h.MarkerEdgeColor = h.Color;

xlabel(".seq file size (MB)");
ylabel("Peak memory (GB)");
title("Peak memory");

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
