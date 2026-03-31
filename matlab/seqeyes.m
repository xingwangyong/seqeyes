function seqeyes(varargin)
    % Wrapper for SeqEyes
    %
    % Use seqeyes to open a .seq file or an in-memory sequence, with options.
    %
    % Usage:
    %   seqeyes                     % Launch SeqEyes with no input
    %   seqeyes(seq)                % Open an mr.Sequence object
    %   seqeyes('scan.seq')         % Open a .seq file by path
    %   seqeyes('--opt', 'scan.seq')  % Options must come before the source
    %   seqeyes('--opt', '`seq`')   % Use base variable; backticks are stripped
    %   seqeyes --opt `seq`         % Shorthand; same as above in concept
    %                               (MATLAB call: seqeyes('--opt','`seq`'))
    %   seqeyes('--help')           % Options-only call

    if ispc
        % Example Windows path:
        % seqeye_exe = 'C:\path\to\seqeyes\out\bin\seqeyes.exe';
        seqeye_exe = 'C:\Users\76494\Downloads\New folder (22)\seqeye\out\build\x64-Debug\seqeyes.exe';
        % seqeye_exe = '__SET_WINDOWS_SEQEYES_PATH__';
    elseif ismac
        % Example macOS app-bundle path from this repo build:
        % seqeye_exe = '/Users/yourname/Code/seqeyes/out/bin/seqeyes.app/Contents/MacOS/seqeyes';
        % folder = '~/seqeyes/';  % Parent folder of seqeyes repo
        % seqeye_exe = fullfile(folder, 'out/bin/seqeyes.app/Contents/MacOS/seqeyes');
        seqeye_exe = '__SET_MACOS_SEQEYES_PATH__';
    else
        % Example Linux path:
        % seqeye_exe = '/home/yourname/seqeyes/out/bin/seqeyes';
        folder = '~/seqeyes/linux';
        seqeye_exe = fullfile(folder, get_linux_major_id,'seqeyes');
        % seqeye_exe = '__SET_LINUX_SEQEYES_PATH__';
    end

    % MATLAB does not reliably expand '~' in file checks/commands.
    seqeye_exe = expand_user_path(seqeye_exe);

    if startsWith(seqeye_exe, '__SET_')
        error(['SeqEyes path is not configured in seqeyes.m. ', ...
               'Please edit get_user_specified_seqeyes_path() for your OS.']);
    end
    
    if ~isfile(seqeye_exe)
        msg = 'SeqEyes executable not found, please try to set its path manually';
        warning(msg);
        warndlg(msg);
        return;
    end
    
    
    % add double quotes to avoid shell expansion
    seqeye_exe = ['"', seqeye_exe, '"'];
    
    
    
    %% Parse inputs
    options = {};
    variable_name = '';
    seq_arg = ''; % Initialize seq_arg
    
    if nargin > 0
        % Inspect the last argument
        last_arg = varargin{end};
        
        if isa(last_arg, 'mr.Sequence')
            % The last argument is a sequence object
            seq_arg = last_arg;
            % All previous arguments are options
            if nargin > 1
                options = varargin(1:end-1);
            end
        elseif ischar(last_arg)
            
            % Check for options-only call (e.g., --help) BEFORE checking .seq
            if startsWith(last_arg, '-')
                fprintf('Options-only call detected.\n');
                seq_arg = ''; % No source
                options = varargin; % All args are options
            
            elseif endsWith(last_arg, '.seq')
                % The last argument is a filename
                seq_arg = last_arg;
                % All previous arguments are options
                if nargin > 1
                    options = varargin(1:end-1);
                end
            else
                % Try backtick-wrapped variable name as source.
                % ... (rest of backtick logic is unchanged) ...
                candidate = last_arg;
                leftIndex = 1;
                while leftIndex <= length(candidate) && candidate(leftIndex) == '`'
                    leftIndex = leftIndex + 1;
                end
                rightIndex = length(candidate);
                while rightIndex >= leftIndex && candidate(rightIndex) == '`'
                    rightIndex = rightIndex - 1;
                end
                if rightIndex >= leftIndex
                    inner = candidate(leftIndex:rightIndex);
                else
                    inner = '';
                end
                removed_any = (leftIndex > 1) || (rightIndex < length(candidate));
                if removed_any && ~isempty(inner)
                    variable_name = inner; % stripped of surrounding backticks
                    if evalin('base', ['exist(''', variable_name, ''', ''var'')'])
                        seq_from_base = evalin('base', variable_name);
                        if ~isa(seq_from_base, 'mr.Sequence')
                            error(['Variable ''%s'' exists in base workspace but is', ...
                                   ' not an mr.Sequence'], variable_name);
                        end
                        seq_arg = seq_from_base;
                        fprintf('Using variable ''%s'' from base workspace\n', ...
                                 variable_name);
                    else
                        error('Variable ''%s'' not found in base workspace', ...
                                variable_name);
                    end
                    % All previous arguments are options
                    if nargin > 1
                        options = varargin(1:end-1);
                    end
                else
                    error(['Last argument must be: (1) seq object, (2) .seq filename, ', ...
                           'or (3) backtick-wrapped variable name like `name`, ``name, ', ...
                           'or name``, or (4) an option like --help']);
                end
            end
        else
            error(['Last argument must be: (1) seq object, (2) .seq filename, ', ...
                   'or (3) backtick-wrapped variable name like `name`, ``name, ', ...
                   'or name``, or (4) an option like --help']);
        end
    else
        % No-argument call: launch the executable without loading a file.
        cmd_args = seqeye_exe;
        cmd_args = [cmd_args, ' &'];
        fprintf('Opening SeqEyes (no input)\n');
        fprintf('Command: %s\n', cmd_args);
        system(cmd_args);
        return;
    end
    
    %% Normalize options (allow values after flags, e.g., --layout 212)
    normalized_options = cell(1, length(options));
    for i = 1:length(options)
        opt_i = options{i};
        if ischar(opt_i)
            normalized_options{i} = opt_i;
        elseif isnumeric(opt_i)
            normalized_options{i} = num2str(opt_i);
        elseif islogical(opt_i)
            normalized_options{i} = char(mat2str(opt_i));
        else
            % Fallback: best-effort to stringify
            try
                normalized_options{i} = char(opt_i);
            catch
                tmp = evalc('disp(opt_i)');
                normalized_options{i} = strtrim(tmp);
            end
        end
    end
    
    %% Handle seq object or filename
    % Handle the options-only case (where seq_arg is empty)
    if isempty(seq_arg)
        seq_fn = '';
        if ~isempty(options)
            fprintf('Using options only, no input file.\n');
        end
    
    elseif isa(seq_arg, 'mr.Sequence')
        % Write a temporary file for the in-memory sequence
        seq_fn = [tempname, '.seq'];
        seq_arg.write(seq_fn);
        fprintf('Using seq object');
        if ~isempty(variable_name)
            fprintf(' (variable: %s)', variable_name);
        end
        fprintf('\n');
    else
        % Use the provided filename directly
        seq_fn = seq_arg;
        
        if ~isfile(seq_fn)
            error('Seq file not found: %s', seq_fn);
        end
        fprintf('Using seq file: %s\n', seq_fn);
    end
    
    %% Prepare command line
    
    % Build command-line arguments
    cmd_args = seqeye_exe;
    
    % Append all options (using normalized strings)
    for i = 1:length(normalized_options)
        tok = normalized_options{i};
        if contains(tok, ' ')
            cmd_args = [cmd_args, ' "', tok, '"'];
        else
            cmd_args = [cmd_args, ' ', tok];
        end
    end
    
    % *** MODIFICATION START ***
    % Only append the sequence filename if it is not empty
    if ~isempty(seq_fn)
        seq_fn_quoted = ['"', seq_fn, '"'];
        cmd_args = [cmd_args, ' ', seq_fn_quoted];
        fprintf('Opening with SeqEyes: %s\n', seq_fn);
    else
        % This path is for options-only calls like --help
        fprintf('Running SeqEyes with options only.\n');
    end
    % *** MODIFICATION END ***
    
    cmd_args = [cmd_args, ' &'];
    
    %% Execute
    fprintf('Command: %s\n', cmd_args);
    system(cmd_args);
    
    end


function out = expand_user_path(in)
%EXPAND_USER_PATH Expand leading '~' to the user's home directory.

    out = in;
    if startsWith(in, '~/') || startsWith(in, '~\')
        out = fullfile(char(java.lang.System.getProperty('user.home')), in(3:end));
    elseif strcmp(in, '~')
        out = char(java.lang.System.getProperty('user.home'));
    end
end
    
    
    function out = get_linux_major_id()
    %GET_LINUX_MAJOR_ID  Return Linux distro + major version, e.g. 'rocky_9'
    %
    %   Only needs /etc/os-release (standard on Rocky/RedHat/Fedora/Ubuntu/Debian).
    
        fname = '/etc/os-release';
        if ~isfile(fname)
            error('/etc/os-release not found. This function only works on Linux.');
        end
    
        txt = fileread(fname);
    
        % Get ID
        id = regexp(txt, 'ID="?([^\n"]+)"?', 'tokens', 'once');
        if isempty(id)
            error('Cannot find ID in /etc/os-release');
        end
        id = id{1};
    
        % Get VERSION_ID
        ver = regexp(txt, 'VERSION_ID="?([^\n"]+)"?', 'tokens', 'once');
        if isempty(ver)
            error('Cannot find VERSION_ID in /etc/os-release');
        end
        ver = ver{1};
    
        % Extract major version number before dot
        major = regexp(ver, '^\d+', 'match', 'once');
        if isempty(major)
            error('VERSION_ID does not contain a major version number.');
        end
    
        % Combine
        out = sprintf('%s_%s', id, major);
    end
    