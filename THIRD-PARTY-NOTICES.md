# Qt 6.8.3

3X Control uses the dynamically linked Qt Core, Gui, Widgets, Network and Sql modules from Qt 6.8.3, Copyright The Qt Company Ltd. and other contributors. These modules are available under the GNU Lesser General Public License version 3, with third-party components under their respective licenses.

The complete LGPL and GPL license texts are supplied in `licenses/`. The unmodified official Qt 6.8.3 SBOM in `licenses/qtbase-6.8.3.spdx.json` identifies Qt's third-party components, copyright notices and embedded license information.

The corresponding library sources, including bundled third-party sources, are available from the Qt distribution used for this build:

https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz

No Qt library source changes were made. You may replace the supplied Qt DLLs with compatible versions, rebuild Qt from that source, and rebuild this application from the accompanying repository with the supplied CMake project. No restriction on reverse engineering for debugging modifications to the LGPL-covered libraries is imposed.

License details: https://www.qt.io/development/open-source-lgpl-obligations

Qt's own source package and SBOM remain authoritative for the licensing terms of its bundled third-party components.

# Microsoft runtime components

The Windows package also contains unmodified app-local Microsoft Visual C++ runtime DLLs from the Visual Studio Redistributable directory and D3Dcompiler_47.dll from the Qt deployment. These are Microsoft components under their respective redistribution terms, separate from Qt's LGPL license.
