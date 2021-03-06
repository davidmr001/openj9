/*******************************************************************************
 * Copyright (c) 2000, 2017 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
 *******************************************************************************/

#include "env/PersistentCHTable.hpp"

#include <stdint.h>                            // for int32_t
#include <stdio.h>                             // for printf, fflush, NULL, etc
#include <string.h>                            // for memcpy, memset, etc
#include "codegen/FrontEnd.hpp"                // for TR_FrontEnd, feGetEnv
#include "compile/Compilation.hpp"             // for Compilation, etc
#include "compile/CompilationTypes.hpp"        // for TR_Hotness
#include "compile/ResolvedMethod.hpp"          // for TR_ResolvedMethod
#include "compile/SymbolReferenceTable.hpp"    // for SymbolReferenceTable
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "env/CHTable.hpp"
#include "env/CompilerEnv.hpp"
#include "env/PersistentInfo.hpp"              // for PersistentInfo
#include "env/RuntimeAssumptionTable.hpp"
#include "env/TRMemory.hpp"
#include "env/jittypes.h"                      // for uintptrj_t
#include "env/ClassTableCriticalSection.hpp"   // for ClassTableCriticalSection
#include "env/VMJ9.h"
#include "il/DataTypes.hpp"                    // for etc
#include "il/SymbolReference.hpp"              // for classNameToSignature, etc
#include "il/symbol/ResolvedMethodSymbol.hpp"  // for ResolvedMethodSymbol
#include "infra/Assert.hpp"                    // for TR_ASSERT
#include "infra/Link.hpp"                      // for TR_LinkHead
#include "infra/List.hpp"                      // for ListIterator, etc
#include "runtime/RuntimeAssumptions.hpp"

class TR_OpaqueClassBlock;

TR_PersistentCHTable::TR_PersistentCHTable(TR_PersistentMemory *trPersistentMemory)
   : _trPersistentMemory(trPersistentMemory)
   {
   /*
    * We want to avoid strange memory allocation failures that might occur in a
    * constructor. This can be done by making  _classes an array that gets
    * allocated as part of the TR_PersistentCHTable  class itself.  But that
    * invokes libstdc++. So making a byte array as part of the
    * TR_PersistentCHTable class and pointing _classes (static type casting) to
    * the byte array memory region, after initializing, also does the same work.
    */

   memset(_buffer, 0, sizeof(TR_LinkHead<TR_PersistentClassInfo>) * (CLASSHASHTABLE_SIZE + 1));
   _classes = static_cast<TR_LinkHead<TR_PersistentClassInfo> *>(static_cast<void *>(_buffer));
   }


void
TR_PersistentCHTable::commitSideEffectGuards(TR::Compilation *comp)
   {
   TR::list<TR_VirtualGuardSite*> *sideEffectPatchSites = comp->getSideEffectGuardPatchSites();
   bool nopAssumptionIsValid = true;

   for (TR_ClassLoadCheck * clc = comp->getClassesThatShouldNotBeLoaded()->getFirst(); clc; clc = clc->getNext())
      {
      for (int32_t i = 0; i < CLASSHASHTABLE_SIZE; ++i)
         {
         for (TR_PersistentClassInfo *pci = _classes[i].getFirst(); pci; pci = pci->getNext())
            {
            int32_t pciCachedNameLength = pci->getNameLength();
            if (!pci->isInitialized()
                || (pciCachedNameLength > -1 && pciCachedNameLength != clc->_length))
               continue;

            TR_OpaqueClassBlock *clazz = pci->getClassId();
            int32_t length;
            char *clazzName = TR::Compiler->cls.classNameChars(comp, clazz, length);
            clazzName = classNameToSignature(clazzName, length, comp);
            if (pciCachedNameLength == -1)
               pci->setNameLength(length);
            if ((length == clc->_length) &&
                !strncmp(clc->_name, clazzName, length))
               {
               nopAssumptionIsValid = false;
               break;
               }
            }
         if (!nopAssumptionIsValid)
            break;
         }

      if (!nopAssumptionIsValid)
         break;
      }

   if (nopAssumptionIsValid)
      {
      for (TR_ClassExtendCheck * cec = comp->getClassesThatShouldNotBeNewlyExtended()->getFirst(); cec; cec = cec->getNext())
         {
         TR_OpaqueClassBlock *clazz = cec->_clazz;

         if (comp->fe()->classHasBeenExtended(clazz))
            {
            TR_PersistentClassInfo * cl = findClassInfo(clazz);
            TR_ScratchList<TR_PersistentClassInfo> subClasses(comp->trMemory());
            TR_ClassQueries::collectAllSubClasses(cl, &subClasses, comp);
            ListIterator<TR_PersistentClassInfo> it(&subClasses);
            TR_PersistentClassInfo *info;
            for (info = it.getFirst(); info; info = it.getNext())
               {
               TR_OpaqueClassBlock *subClass = info->getClassId();
               bool newSubClass = true;
               for (TR_ClassExtendCheck *currCec = comp->getClassesThatShouldNotBeNewlyExtended()->getFirst(); currCec; currCec = currCec->getNext())
                  {
                  if (subClass == currCec->_clazz)
                     {
                     newSubClass = false;
                     break;
                     }
                  }

               if (newSubClass)
                  {
                  break;
                  }
               }

            if (info)
               {
               nopAssumptionIsValid = false;
               break;
               }
            }
         }
      }

   if (nopAssumptionIsValid)
      {
      for (TR_ClassLoadCheck * clc = comp->getClassesThatShouldNotBeLoaded()->getFirst(); clc; clc = clc->getNext())
         {
         for (auto site = sideEffectPatchSites->begin(); site != sideEffectPatchSites->end(); ++site)
            {
            TR_ASSERT((*site)->getLocation(), "assertion failure");
            TR_PatchNOPedGuardSiteOnClassPreInitialize::make
               (comp->fe(), comp->trPersistentMemory(), clc->_name, clc->_length, (*site)->getLocation(),
                (*site)->getDestination(), comp->getMetadataAssumptionList());
            comp->setHasClassPreInitializeAssumptions();
            }
         }

      for (TR_ClassExtendCheck * cec = comp->getClassesThatShouldNotBeNewlyExtended()->getFirst(); cec; cec = cec->getNext())
         {
         TR_OpaqueClassBlock *clazz = cec->_clazz;
         TR_PersistentClassInfo * cl = findClassInfo(clazz);

         for (auto site = sideEffectPatchSites->begin(); site != sideEffectPatchSites->end(); ++site)
            {
            TR_ASSERT((*site)->getLocation(), "assertion failure");
            if (cl)
               {
               TR_PatchNOPedGuardSiteOnClassExtend::make(comp->fe(), comp->trPersistentMemory(), clazz,
                                                                        (*site)->getLocation(),
                                                                        (*site)->getDestination(),
                                                                        comp->getMetadataAssumptionList());
               comp->setHasClassExtendAssumptions();
               }
            else
               TR_ASSERT(0, "Could not find class info for class that should not be newly extended\n");
            }
         }
      }
   else
      {
      for (auto site = sideEffectPatchSites->begin(); site != sideEffectPatchSites->end(); ++site)
         TR::PatchNOPedGuardSite::compensate(0, (*site)->getLocation(), (*site)->getDestination());
      }
   }


// class used to determine the only jitted implementer of a virtual method
class FindSingleJittedImplementer : public TR_SubclassVisitor
   {
   public:

   FindSingleJittedImplementer(
         TR::Compilation * comp,
         TR_OpaqueClassBlock *topClassId,
         TR_ResolvedMethod *callerMethod,
         int32_t slotOrIndex) :
      TR_SubclassVisitor(comp)
      {
      _topClassId = topClassId;
      _implementer = 0;
      _callerMethod = callerMethod;
      _slotOrIndex = slotOrIndex;
      _topClassIsInterface = TR::Compiler->cls.isInterfaceClass(comp, topClassId);
      _maxNumVisitedSubClasses = comp->getOptions()->getMaxNumVisitedSubclasses();
      _numVisitedSubClasses = 0;
      }

   virtual bool visitSubclass(TR_PersistentClassInfo *cl);

   TR_ResolvedMethod *getJittedImplementer() const { return _implementer; }

   private:

   TR_OpaqueClassBlock *_topClassId;
   TR_ResolvedMethod *_implementer;
   TR_ResolvedMethod *_callerMethod;
   int32_t _slotOrIndex;
   bool _topClassIsInterface;
   int32_t _maxNumVisitedSubClasses;
   int32_t _numVisitedSubClasses;
   };


bool
FindSingleJittedImplementer::visitSubclass(TR_PersistentClassInfo *cl)
   {
   TR_OpaqueClassBlock * classId = cl->getClassId();

   if (!TR::Compiler->cls.isAbstractClass(comp(), classId) && !TR::Compiler->cls.isInterfaceClass(comp(), classId))
      {
      TR_ResolvedMethod *method;
      if (_topClassIsInterface)
         method = _callerMethod->getResolvedInterfaceMethod(comp(), classId, _slotOrIndex);
      else
         method = _callerMethod->getResolvedVirtualMethod(comp(), classId, _slotOrIndex);

      ++_numVisitedSubClasses;
      if ((_numVisitedSubClasses > _maxNumVisitedSubClasses) || !method)
         {
         stopTheWalk();
         _implementer = 0; // signal failure
         return false;
         }

      // check to see if there are any duplicates
      if (!method->isInterpreted())
         {
         if (_implementer)
            {
            if (!method->isSameMethod(_implementer))
               {
               //found two compiled implementors
               stopTheWalk();
               _implementer = 0; // signal failure
               return false;
               }
            }
         else // add this compiled implementer to the list
            {
            _implementer = method;
            }
         }
      }
   return true;
   }


TR_ResolvedMethod *
TR_PersistentCHTable::findSingleJittedImplementer(
      TR_OpaqueClassBlock *thisClass,
      int32_t vftSlot,
      TR_ResolvedMethod *callerMethod,
      TR::Compilation *comp,
      TR::ResolvedMethodSymbol *calleeSymbol,
      bool locked)
   {
   if (comp->fej9()->isAOT_DEPRECATED_DO_NOT_USE())
      return 0;

   if (comp->getOption(TR_DisableCHOpts))
      return 0;

   if (comp->getSymRefTab()->findObjectNewInstanceImplSymbol() &&
       comp->getSymRefTab()->findObjectNewInstanceImplSymbol()->getSymbol() == calleeSymbol)
      return 0;

   TR_ResolvedMethod *resolvedMethod = NULL;

      {
      TR::ClassTableCriticalSection findSingleJittedImplementer(comp->fe(), locked);

      TR_PersistentClassInfo * classInfo = findClassInfo(thisClass);
      if (!classInfo)
         return 0;

      FindSingleJittedImplementer collector(comp, thisClass, callerMethod, vftSlot);
      collector.visitSubclass(classInfo);
      collector.visit(thisClass, true);

      resolvedMethod = collector.getJittedImplementer();
      }

   return resolvedMethod;
   }


TR_PersistentClassInfo *
TR_PersistentCHTable::findClassInfo(TR_OpaqueClassBlock * classId)
   {
   TR_PersistentClassInfo *cl = _classes[TR_RuntimeAssumptionTable::hashCode((uintptrj_t)classId) % CLASSHASHTABLE_SIZE].getFirst();
   while (cl &&
          cl->getClassId() != classId)
      cl = cl->getNext();
   return cl;
   }


TR_PersistentClassInfo *
TR_PersistentCHTable::findClassInfoAfterLocking(
      TR_OpaqueClassBlock *classId,
      TR::Compilation *comp,
      bool returnClassInfoForAOT)
   {
   if (comp->fej9()->isAOT_DEPRECATED_DO_NOT_USE() && !returnClassInfoForAOT) // for AOT do not use the class hierarchy
      return NULL;

   if (comp->getOption(TR_DisableCHOpts))
      return NULL;

   TR_PersistentClassInfo * cl = NULL;

      {
      TR::ClassTableCriticalSection findClassInfoAfterLocking(comp->fe());
      cl = findClassInfo(classId);
      }

   return cl;
   }


bool
TR_PersistentCHTable::isOverriddenInThisHierarchy(
      TR_ResolvedMethod *method,
      TR_OpaqueClassBlock *thisClass,
      int32_t vftSlot,
      TR::Compilation *comp,
      bool locked)
   {
   if (comp->getOption(TR_DisableCHOpts))
      return true; // fake answer to disable any optimizations based on CHTable

   if (thisClass == method->classOfMethod())
      return method->virtualMethodIsOverridden();

   TR_PersistentClassInfo * classInfo = findClassInfoAfterLocking(thisClass, comp);
   if (!classInfo)
      return true;

   TR_J9VMBase *fej9 = (TR_J9VMBase *)(method->fe());

   if (debug("traceOverriddenInHierarchy"))
      {
      printf("virtual method %s\n", method->signature(comp->trMemory()));
      printf("offset %d\n", vftSlot);
      int32_t len; char * s = TR::Compiler->cls.classNameChars(comp, thisClass, len);
      printf("thisClass %.*s\n", len, s);
      }

   if (fej9->getResolvedVirtualMethod(thisClass, vftSlot) != method->getPersistentIdentifier())
      return true;

   if (!fej9->classHasBeenExtended(thisClass))
      return false;

   TR_ScratchList<TR_PersistentClassInfo> leafs(comp->trMemory());
   TR_ClassQueries::collectLeafs(classInfo, leafs, comp, locked);
   ListIterator<TR_PersistentClassInfo> i(&leafs);
   for (classInfo = i.getFirst(); classInfo; classInfo = i.getNext())
      {
      if (debug("traceOverriddenInHierarchy"))
         {
         int32_t len; char * s = TR::Compiler->cls.classNameChars(comp, classInfo->getClassId(), len);
         printf("leaf %.*s\n", len, s);
         }
      if (fej9->getResolvedVirtualMethod(classInfo->getClassId(), vftSlot) != method->getPersistentIdentifier())
         return true;
      }
   return false;
   }

TR_ResolvedMethod * TR_PersistentCHTable::findSingleImplementer(
   TR_OpaqueClassBlock * thisClass, int32_t cpIndexOrVftSlot, TR_ResolvedMethod * callerMethod, TR::Compilation * comp, bool locked, TR_YesNoMaybe useGetResolvedInterfaceMethod)
   {
   if (comp->getOption(TR_DisableCHOpts))
      return 0;



   TR_PersistentClassInfo * classInfo = comp->getPersistentInfo()->getPersistentCHTable()->findClassInfoAfterLocking(thisClass, comp, true);
   if (!classInfo)
      {
      return 0;
      }

   TR_ResolvedMethod *implArray[2]; // collect maximum 2 implemeters if you can
   int32_t implCount = TR_ClassQueries::collectImplementorsCapped(classInfo, implArray, 2, cpIndexOrVftSlot, callerMethod, comp, locked, useGetResolvedInterfaceMethod);
   return (implCount == 1 ? implArray[0] : 0);
   }

TR_ResolvedMethod *
TR_PersistentCHTable::findSingleInterfaceImplementer(
      TR_OpaqueClassBlock *thisClass,
      int32_t cpIndex,
      TR_ResolvedMethod *callerMethod,
      TR::Compilation *comp,
      bool locked)
   {
   if (comp->getOption(TR_DisableCHOpts))
      return 0;

   if (!TR::Compiler->cls.isInterfaceClass(comp, thisClass))
      {
      return 0;
      }

   TR_PersistentClassInfo * classInfo = findClassInfoAfterLocking(thisClass, comp, true);
   if (!classInfo)
      {
      return 0;
      }

   TR_ResolvedMethod *implArray[2]; // collect maximum 2 implemeters if you can
   int32_t implCount = TR_ClassQueries::collectImplementorsCapped(classInfo, implArray, 2, cpIndex, callerMethod, comp, locked);
   return (implCount == 1 ? implArray[0] : 0);
   }

bool
TR_PersistentCHTable::hasTwoOrMoreCompiledImplementors(
   TR_OpaqueClassBlock * thisClass, int32_t cpIndex, TR_ResolvedMethod * callerMethod, TR::Compilation * comp, TR_Hotness hotness, bool locked)
   {
   if (comp->getOption(TR_DisableCHOpts))
      return false;

   if (!TR::Compiler->cls.isInterfaceClass(comp, thisClass))
      return false;

   TR_PersistentClassInfo * classInfo = findClassInfoAfterLocking(thisClass, comp, true);
   if (!classInfo) return false;

   TR_ResolvedMethod *implArray[2];
   return TR_ClassQueries::collectCompiledImplementorsCapped(classInfo,implArray,2,cpIndex,callerMethod,comp,hotness,locked) == 2;
   }

int32_t
TR_PersistentCHTable::findnInterfaceImplementers(
      TR_OpaqueClassBlock *thisClass,
      int32_t n,
      TR_ResolvedMethod *implArray[],
      int32_t cpIndex,
      TR_ResolvedMethod *callerMethod,
      TR::Compilation *comp,
      bool locked)
   {
   if (comp->getOption(TR_DisableCHOpts))
      return 0;

   if (!TR::Compiler->cls.isInterfaceClass(comp, thisClass))
      return 0;

   TR_PersistentClassInfo * classInfo = findClassInfoAfterLocking(thisClass, comp, true);
   if (!classInfo) return 0;

   int32_t implCount = TR_ClassQueries::collectImplementorsCapped(classInfo, implArray,n,cpIndex,callerMethod,comp,locked);
   return implCount;
   }


bool
TR_PersistentCHTable::isKnownToHaveMoreThanTwoInterfaceImplementers(
      TR_OpaqueClassBlock *thisClass,
      int32_t cpIndex,
      TR_ResolvedMethod *callerMethod,
      TR::Compilation * comp,
      bool locked)
   {
   if (comp->getOption(TR_DisableCHOpts))
      return true; // conservative answer if we want to disable optimization based on CHtable

   TR_PersistentClassInfo * classInfo = findClassInfoAfterLocking(thisClass, comp);
   if (!classInfo)
      return false;

   TR_ResolvedMethod *implArray[3]; // collect maximum 3 implemeters if you can
   int32_t implCount = TR_ClassQueries::collectImplementorsCapped(classInfo, implArray, 3, cpIndex, callerMethod, comp, locked);
   return (implCount == 3);
   }


TR_ResolvedMethod *
TR_PersistentCHTable::findSingleAbstractImplementer(
   TR_OpaqueClassBlock * thisClass, int32_t vftSlot, TR_ResolvedMethod * callerMethod, TR::Compilation * comp, bool locked)
   {
   if (comp->getOption(TR_DisableCHOpts))
      return 0;
   TR_PersistentClassInfo * classInfo = findClassInfoAfterLocking(thisClass, comp);
   if (!classInfo) return 0;

   if (TR::Compiler->cls.isInterfaceClass(comp, thisClass))
      return 0;

   TR_ResolvedMethod *implArray[2]; // collect maximum 2 implemeters if you can
   int32_t implCount = TR_ClassQueries::collectImplementorsCapped(classInfo, implArray, 2, vftSlot, callerMethod, comp, locked);
   return (implCount == 1 ? implArray[0] : 0);
   }


TR_OpaqueClassBlock *
TR_PersistentCHTable::findSingleConcreteSubClass(
      TR_OpaqueClassBlock *opaqueClass,
      TR::Compilation *comp)
   {
   TR_OpaqueClassBlock *concreteSubClass = NULL;
   if (comp->getOption(TR_DisableCHOpts))
      return 0;

   TR_PersistentClassInfo *classInfo = comp->getPersistentInfo()->getPersistentCHTable()->findClassInfoAfterLocking(opaqueClass, comp);
   if (classInfo)
      {
      TR_ScratchList<TR_PersistentClassInfo> subClasses(comp->trMemory());
      TR_ClassQueries::collectAllSubClasses(classInfo, &subClasses, comp);
      ListIterator<TR_PersistentClassInfo> subClassesIt(&subClasses);
      for (TR_PersistentClassInfo *subClassInfo = subClassesIt.getFirst(); subClassInfo; subClassInfo = subClassesIt.getNext())
         {
         TR_OpaqueClassBlock *subClass = (TR_OpaqueClassBlock *) subClassInfo->getClassId();
         if (!TR::Compiler->cls.isAbstractClass(comp, subClass) && !TR::Compiler->cls.isInterfaceClass(comp, subClass))
            {
            if (concreteSubClass)
               return NULL;
            concreteSubClass = subClass;
            }
         }
      }

   return concreteSubClass;
   }


#ifdef DEBUG
void
TR_PersistentCHTable::dumpStats(TR_FrontEnd * fe)
   {
   }
#endif


void
TR_PersistentCHTable::dumpMethodCounts(TR_FrontEnd *fe, TR_Memory &trMemory)
   {
   TR_J9VMBase *fej9 = (TR_J9VMBase *)fe;
   for (int32_t i = 0; i < CLASSHASHTABLE_SIZE; i++)
      {
      for (TR_PersistentClassInfo *pci = _classes[i].getFirst(); pci; pci = pci->getNext())
         {
         TR_ScratchList<TR_ResolvedMethod> resolvedMethodsInClass(&trMemory);
         fej9->getResolvedMethods(&trMemory, pci->getClassId(), &resolvedMethodsInClass);
         ListIterator<TR_ResolvedMethod> resolvedIt(&resolvedMethodsInClass);
         TR_ResolvedMethod *resolvedMethod;
         for (resolvedMethod = resolvedIt.getFirst(); resolvedMethod; resolvedMethod = resolvedIt.getNext())
            {
            const char *signature = resolvedMethod->signature(&trMemory);
            int32_t count = resolvedMethod->getInvocationCount();

            printf("Final: Signature %s Count %d\n",signature,count);fflush(stdout);
            }
         }
      }
   }


void
TR_PersistentCHTable::resetVisitedClasses() // highly time consumming
   {
   for (int32_t i = 0; i <= CLASSHASHTABLE_SIZE; ++i)
      {
      TR_PersistentClassInfo * cl = _classes[i].getFirst();
      while (cl)
        {
        cl->resetVisited();
        cl = cl->getNext();
        }
      }
   }



void
TR_PersistentCHTable::classGotUnloaded(
      TR_FrontEnd *fe,
      TR_OpaqueClassBlock *classId)
   {
   TR_PersistentClassInfo * cl = findClassInfo(classId);

   bool p = TR::Options::getVerboseOption(TR_VerboseHookDetailsClassUnloading);
   if (p)
      {
      TR_VerboseLog::writeLineLocked(TR_Vlog_HD, "setting class 0x%p as unloaded\n", classId);
      }

   //if the class was not fully loaded, it might not be in the RAT.
   if(cl)
      cl->setUnloaded();
   }


TR_PersistentClassInfo *
TR_PersistentCHTable::classGotLoaded(
      TR_FrontEnd *fe,
      TR_OpaqueClassBlock *classId)
   {
   TR_ASSERT(!findClassInfo(classId), "Should not add duplicates to hash table\n");
   TR_PersistentClassInfo *clazz = new (PERSISTENT_NEW) TR_PersistentClassInfo(classId);
   if (clazz)
      {
      _classes[TR_RuntimeAssumptionTable::hashCode((uintptrj_t) classId) % CLASSHASHTABLE_SIZE].add(clazz);
      }
   return clazz;
   }
